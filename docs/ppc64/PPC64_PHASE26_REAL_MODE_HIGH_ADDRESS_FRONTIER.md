# PPC64 Phase 26: POWER9 Real-Mode High-Address Frontier

## Outcome

**Outcome A: PPC64-26 accepted.** The production guest path now forms the POWER9 guest-real address from a qualified 64-bit effective address and then uses the active LPID partition radix translation. The unchanged Linux image reproduced its previous 46 retirements, fetched and retired the high-address `bl` at `0xC00000000000D060` through `VirtualMachine::step()`, and continued to a reproducible bounded endpoint after 128 instructions.

The run did not boot Linux to completion. The final observation is a budget endpoint, not a runtime failure.

## Repository and artifact state

- Required branch: `PPC64`.
- Starting HEAD: `c9101f2b210a0bc5da4fea7988460b8cb1cc26eb`.
- Starting divergence: `origin/PPC64`, 6 commits ahead and 0 behind.
- Starting worktree: clean; the ignored `build/` directory was preserved.
- Phase marker before this work: `last_completed=PPC64-25`, `next_expected=PPC64-26`.
- Linux run artifacts: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase26\<run-id>\`.
- QEMU runtime remains `QEMU_REFERENCE_PENDING`; no QEMU/QAC process was launched.

The most recent passing run is `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase26\run-1793709588815\`. Its manifest is `artifact-sha256-manifest.txt`; it records these key evidence hashes:

- Bounded production trace: `linux-bounded-trace.txt`, SHA-256 `4c520517e9b4355b5ef315d0c6a625ea9142b64508ef9c72f427909a8d69d1d9`.
- Frontier report: `linux-frontier-report.txt`, SHA-256 `9d6869a0c28584c4b42ef9c12908e7faed179f6e7b30549c981ed96d58fcc1dc`.
- Two-run comparison: `two-run-comparison.txt`, SHA-256 `3ee8bdbcd37933269f9b2c2bf5e044747bfc5aeed899588cd05e608997f6205c`.
- Focused address vectors: `real-mode-address-vectors.txt`, SHA-256 `f1ce68e356629ca5d676a96b313e7f43989c9effcee73d063b04f4cba91ef31a`.
- Authentic high-address frontier: `authentic-linux-frontier.txt`, SHA-256 `565253535291a551c1590532b5c349ef8ff82b6770fe94d177c0dcd7208e56b5`.

The run directory also preserves the source comparison, address contract table, pre-repair trace, artifact verification, focused vector results, FDT, and SHA-256 manifest. The kernel, FDT, and prior Phase 25 artifacts remain outside Git.

## Original frontier and reconstructed state

Phase 25 had retired 46 instructions. Its trace and frontier report remain unchanged and were checked against the recorded hashes:

- Phase 25 trace: `bcadaf88cb8a2b7282663896b8a94a5eb6e96c8317a1d0c4b20afe2be31032d6`.
- Phase 25 frontier: `932e087b8aa92bf1c71fa984c64e4f62838d9c4f157642620bdf9c1583a71a2c`.

The transition sequence in the new production trace is:

| NIP | Instruction | Evidence |
|---|---|---|
| `0xD024` | `rldicl r25,r25,16,0` | `r25: 0xD060 -> 0xD0600000`, next NIP `0xD028` |
| `0xD02C` | `rldicl r25,r25,48,0` | `r25: 0xD060C000 -> 0xC00000000000D060`, next NIP `0xD030` |
| `0xD030` | authentic `mtspr` | Raw word, operands, LR/CTR, and MSR are in each trace record |
| `0xD034` | `bclr` (`0x4E800020`) | Retired to `0xC00000000000D060`; pre-branch LR `0xC00000000000D060`, CTR `0`, MSR `0x8000000000000000` |

The high NIP is produced by Linux's retired RLDICL and BCLR sequence. The harness does not set the Linux NIP to manufacture the frontier. Each of the first 46 trace records includes raw instruction, decode, before/after MSR, LR, CTR, CR/XER, and relevant register state. The two-run test compares those records to Phase 25.

At the attempted high fetch the guest mode is `SF=1, HV=0, IR=0, DR=0, PR=0`. Phase 25 had no active partition context or PATE setup; its default active LPID value was zero without a configured root partition. The raw no-context path therefore called `Memory::Read(0xC00000000000D060)`, which exceeded the 128 MiB backing allocation. The saved Phase 25 frontier reports a `FETCH_FAILURE`, 46 retired instructions, and the candidate only as ELF-linked bytes.

The first unsupported decision was the no-context branch in `PPC64ISAPlugin::decode()` that read the effective NIP directly from backing memory. `Memory::Read()` correctly rejected that out-of-range offset. The Phase 26 Linux guest setup now supplies the missing LPID 1 partition context. The address is then normalized and translated before a backing read; RAM was not enlarged and the kernel placement was not changed.

## POWER9 address contract selected

The source comparison uses OpenPOWER Power ISA 3.1C, Linux v7.2 source commit `8d3ae59288f1e7d58d76558a6ee96d533bc5019f`, and QEMU source commit `74f7bf4c59e6b6d50dbc6e124d93440be8be4386`. The QEMU radix implementation has a guest non-HV relocation-off path that ignores the top four effective-address bits and then continues through partition-scoped translation. Its hypervisor-real and virtual-hypervisor flat-real paths are separate, including their HRMOR-related behavior. The Linux pSeries entry source describes entry with translation disabled and execution at the physical boot address.

References:

- [OpenPOWER ISA specifications](https://openpowerfoundation.org/specifications/isa/)
- [Pinned Linux v7.2 `head_64.S`](https://raw.githubusercontent.com/torvalds/linux/8d3ae59288f1e7d58d76558a6ee96d533bc5019f/arch/powerpc/kernel/head_64.S)
- [Pinned Linux v7.2 `reg.h`](https://raw.githubusercontent.com/torvalds/linux/8d3ae59288f1e7d58d76558a6ee96d533bc5019f/arch/powerpc/include/asm/reg.h)
- [Pinned QEMU radix MMU](https://raw.githubusercontent.com/qemu/qemu/74f7bf4c59e6b6d50dbc6e124d93440be8be4386/target/ppc/mmu-radix64.c)
- [Pinned QEMU hash/real-mode MMU](https://raw.githubusercontent.com/qemu/qemu/74f7bf4c59e6b6d50dbc6e124d93440be8be4386/target/ppc/mmu-hash64.c)
- [Pinned QEMU common MMU](https://raw.githubusercontent.com/qemu/qemu/74f7bf4c59e6b6d50dbc6e124d93440be8be4386/target/ppc/mmu_common.c)

The selected Linux profile is specifically a guest with `HV=0`, `SF=1`, `IR=0`, `DR=0`, and active LPID 1. In that context:

```text
guest-real address = effective address & 0x0FFFFFFFFFFFFFFF
0xC00000000000D060 & 0x0FFFFFFFFFFFFFFF = 0xD060
```

`0xD060` is a guest-real address, not a host pointer. The active LPID's partition radix walk then resolves it to backing `0xD060`. There is no unconditional address mask: translated mode still uses process radix, and the hypervisor path remains a separate constrained host-real region.

| Context | Supported behavior in this VM |
|---|---|
| `HV=1, IR=0` | Only the configured privileged host-real handler range; no low-60 alias or general HRMOR implementation |
| `HV=0, IR=0, SF=1`, LPID 0 | Host-configured root PATE, low-60 conversion, partition radix, registered root backing ownership |
| `HV=0, IR=0, SF=1`, non-root LPID | Low-60 conversion, then that LPID's PATE/radix mapping and backing ownership checks |
| `HV=0, IR=0` without a partition or flat radix context | Legacy low-address raw-memory compatibility; no high alias |
| `HV=0, IR=1` | Existing process radix selects the guest-real address; active partition context adds partition translation |
| `HV=1, IR=1` | Unsupported by this bounded VM profile |
| `DR=0, SF=1`, active guest LPID | The same low-60 guest-real conversion and partition walk as fetch |
| `DR=1` | Existing process radix path; a valid process and backing context are required |

LPID 0 and non-root LPIDs use host-created PATE contexts. The Phase 26 vectors prove root and LPID 2 executable backing, and reject attempts by LPID 1 to alias either private backing range. This does not add automatic per-CPU LPID banking.

## Implementation and safety

`src/isa/PPC64ISAPlugin.cpp` adds one low-60 conversion helper gated by `MSR[SF]`. It is used only by the guest real-mode instruction-fetch path when a partition/flat context exists, the ordinary data real-mode path, and the reservation real-mode path. The fetch branch with `IR=1` remains on `translateAccessRange()`. The data branch with `DR=1` remains on the process translation helpers. The hypervisor fetch/load/store path and the no-context raw compatibility path remain separate.

After conversion, the existing translation helpers still validate the complete access width, overflow, page permissions, execute permission, backing RAM bounds, protected hypervisor backing, and LPID-private backing ranges. Cross-page writes are prevalidated and committed transactionally. Fault records retain the original high effective address. Reservation keys use the resolved backing address, so a high-address `lwarx` and low guest-real `stwcx.` alias share the same reservation.

The relevant code paths are `decode()` at `src/isa/PPC64ISAPlugin.cpp:2121`, the conversion helper near line 28, ordinary load/store near line 2523, reservation execution near line 3010, and partition real-range validation near line 4716. The raw memory bounds check is in `src/memory/memory.cpp:49`; host-created LPID contexts are managed in `src/isa/PPC64PartitionMMU.cpp:1011`.

Exception classification stays in the existing mode-specific paths. Process translation faults can be delivered as guest ISI/DSI; partition-stage storage faults in a non-root LPID use HISI/HDSI delivery to the hypervisor. The new vectors inspect the underlying fault categories and confirm that NIP and memory effects remain precise. Existing Phases 6–9 and 19 cover the delivered-vector behavior. The Phase 26 no-delivery fault vectors assert the recorded fault and unchanged faulting NIP.

## Independent address and fault vectors

`PPC64Phase26Tests` adds 111 real-mode and mode-separation assertions, including:

- High and low LPID 1 instruction fetch to the same backing page; high effective fetch and retirement use the CPU plugin.
- A valid instruction word at low memory does not bypass a missing high-address partition leaf.
- `SF=0` does not gain the qualified 64-bit alias; no-context guest mode still uses raw low-address access; configured HV real mode does not use the guest alias.
- LPID 0, LPID 1, and LPID 2 use their own PATE and private backing ranges.
- Read-only data remains readable but rejects stores; non-executable backing rejects instruction fetch; protected hypervisor backing and out-of-RAM backing fault.
- Cross-page 8-byte loads and stores traverse non-contiguous backing pages. A denied second store page leaves both pages unchanged.
- High `lwarx` followed by low-address `stwcx.` uses the same backing reservation.
- Aligned instruction fetch at the final word of a 4 KiB page succeeds; a misaligned fetch faults. With fixed 4-byte instructions and 4 KiB pages, no legal aligned instruction can straddle a page boundary.
- Valid high `IR=1` fetch uses process radix; a missing process PTE faults without a real-mode fallback. `IR` and `DR` are exercised independently, and `MTMSRD` plus `ISYNC` changes the subsequent fetch path.
- A translated data access wrapping `UINT64_MAX` faults with the original EA preserved.

Invalid PTE, execute denial, partition permission denial, invalid backing, and address wrap are distinguished by their translation-fault metadata. Existing tests cover the wider guest-vector and cross-VM isolation contracts.

## Authentic Linux image and execution

The replay uses the unchanged authenticated kernel and original direct-FDT entry setup:

- Kernel SHA-256: `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd`.
- Config SHA-256: `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa`.
- FDT SHA-256: `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37`.
- PVR: `0x004E1200`.
- ELF physical range: `[0x0, 0x29C2158)`.
- Initial NIP `0`, MSR `0x8000000000000000`, `r3=0x07FF0000`, `r4=0`, `r5=0`, RTAS disabled.

The Phase 26 harness creates LPID 1 with a POWER9 radix PATE and 4 KiB identity backing leaves covering the existing 128 MiB RAM. It verifies the immutable kernel through the existing ELF manifest verifier, compares the FDT identity, and records the actual fetched word from production decode. The direct backing read is only a secondary byte comparison.

At effective NIP `0xC00000000000D060`, production decode fetched `0x480002D5` from guest-real/backing `0xD060`. Independent decoding identifies `bl 0xC00000000000D334`. `VirtualMachine::step()` retired it; LR became `0xC00000000000D064` and NIP became `0xC00000000000D334`.

The fresh VM runs agreed on the first 46 retirement records, FDT, memory plan, entry registers, LPID, translation profile, virtual time, candidate bytes, branch retirement, LR/NIP, continuation trace, and next frontier. Both started at virtual time 0 and retired 128 instructions at one tick each. The last retired instruction was at `0xC000000000053848` (`0x41820038`). A non-retired production lookahead fetched `0x2C260007` at `0xC00000000005384C`, decoded independently as `cmpdi r6,7`, and resolved it to guest-real/backing `0x5384C`. This is a bounded endpoint; the instruction has not yet been executed in this phase.

## Test results and artifacts

- Phase 26 acceptance suite: 1/1 CTest passed, 1,004 assertions, 0 failures.
- Accepted Phases 1–25 baseline: 14,443 assertions; every corresponding CTest suite passed in the full run.
- Full CTest: 34 passed, 0 executed failures, 6 `Not Run` because shared executables were absent (`SyscallTests`, `DebuggerTests`, `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, `ELFValidationTests`). The CTest command exits nonzero for these missing executables; none was built as part of this phase.
- `VMCheckpointTests` and `IA64SalTests` passed in CTest.
- IA-64 ACPI, interrupt controller, and Processor Interrupt Block standalone executables all passed.
- Historical Phase 16 timing behavior was not altered; the phase passed during full CTest.

The Phase 26 run directory contains the source comparison, address contract table, pre-repair frontier, focused vector results, two-run comparison, Linux trace, frontier, manifests, FDT, and SHA-256 manifest. Important hashes from the passing run are recorded in `artifact-sha256-manifest.txt`; Phase 25's prior artifacts were preserved.

## Programs A–N

All acceptance programs passed in the verified run:

A. Authentic 46-instruction frontier reconstruction and high BCLR.
B. POWER9 guest-real plus LPID contract comparison.
C. High effective-address conversion through partition radix.
D. Low guest-real address consistency.
E. IR/DR translation separation and translated missing-page behavior.
F. Root and non-root LPID ownership checks.
G. Read, write, execute, protected-range, and private-backing checks.
H. Cross-page data, alignment boundary, backing bounds, and overflow checks.
I. Fault metadata, original EA, unchanged NIP, and write atomicity.
J. `MTMSRD`/`ISYNC` relocation transition.
K. Authentic Linux high-address fetch through production decode.
L. Authentic `bl` retirement through `VirtualMachine::step()`.
M. Two fresh production VMs with identical evidence.
N. Bounded continuation and independently decoded next frontier.

## Remaining limits and PPC64-27 recommendation

This phase establishes only the observed POWER9 guest real-mode address profile. It does not implement general HRMOR, hash-MMU expansion, unrestricted hypervisor real mode, `HV=1` translated execution, CPU LPID banking, or a full Linux boot. QEMU runtime equivalence remains unclaimed.

PPC64-27 should continue from the reproducible budget endpoint at `0xC00000000005384C`, execute the already-decoded `cmpdi r6,7` through the production VM, and extend the bounded trace until it captures the next actual fault or agreed instruction budget. Do not add an instruction based on lookahead alone; the current lookahead decodes successfully and is not yet an observed blocker.
