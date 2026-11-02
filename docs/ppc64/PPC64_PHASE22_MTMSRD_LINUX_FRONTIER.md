# PPC64 Phase 22 — MTMSRD and Linux Frontier

## Result

**Outcome C — the equivalent PPC64-22 implementation was already present at the actual checkout HEAD.** The implementation commit `d24fead0c647ccf33d8604ca42ef5ca98430efa2` already contained MTMSRD, its focused tests, replay evidence, and the advanced PPC64 marker. This verification pass retained that implementation, reran the focused and regression suites, replayed the authentic kernel, corrected two stale assertions in earlier phase tests, and committed those compatibility updates locally. The original implementation's architectural and replay results are documented below.

The implementation covers the POWER9/pSeries foundation profile. It does not implement 32-bit effective-address execution or claim a complete POWER9 hypervisor architecture.

## Original implementation commit's starting repository state

- Repository: `D:\dev\guideXOS_Hypervisor_PPC64`
- Branch: `PPC64`
- Starting HEAD: `460583d1e9fe3919f314cd385451ac07ca8e66ea`
- Upstream divergence before this work: one local commit ahead, zero behind.
- Tracked worktree was clean at preflight. The ignored `build/` directory was retained.
- Initial `.phase-ppc64`: `last_completed=PPC64-21`, `next_expected=PPC64-22`.
- Shared IA-64 `.phase` was not modified.

## Verification pass for the existing implementation

- Actual checkout at the start of this request: HEAD `d24fead0c647ccf33d8604ca42ef5ca98430efa2`, branch `PPC64`, two local commits ahead of `origin/PPC64`, zero behind. Its parent was the requested Phase 21 starting HEAD `460583d1e9fe3919f314cd385451ac07ca8e66ea`.
- The tracked tree was clean before revalidation; ignored `build/` was retained. `.phase-ppc64` already recorded PPC64-22 complete and PPC64-23 next. The shared IA-64 `.phase` remained untouched.
- Phase 4's old tests still expected MTMSRD to be unsupported. They now assert the Phase 22 decode and privileged no-op retirement. Phase 20's replay test used a numeric NIP comparison that rejected a valid branch to a lower address after crossing `0xD320`; it now rejects only stopping at the old blocker.
- Focused Phase 22 CTest passed 278 checks with zero failures. The Phase 4 suite passed 378 checks, and Phase 20 passed 99 checks; its authentic replay again reached the PVR frontier at `0xCFF4`.
- Full configured CTest: 36 entries, 30 passed, zero executed failures, six Not Run because the Syscall, Debugger, Profiler, Fuzzer, VMIsolation, and ELFValidation executables are absent. All configured PPC64 Phase 1–22 suites passed. Standalone IA-64 ACPI, interrupt-controller, and Processor Interrupt Block tests passed; IA-64 SAL passed in CTest.
- A fresh pair of production VM replays is preserved under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase22\run-1793643298714\`. Both runs matched; the bounded trace and frontier-summary hashes match the values below. No kernel, FDT, loader, or memory plan was changed.
- QEMU runtime and QAC were not run. The full all-target build was not repeated in this verification pass; all PPC64 Phase 1–22 test targets were rebuilt successfully.

## Original Linux frontier and input identity

The unchanged Debian PPC64BE kernel is:

`E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\candidate\vmlinuz-7.2.14+deb14-powerpc64`

Its SHA-256 is `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd`. The kernel configuration SHA-256 is `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa`. The existing direct-FDT bytes hash to `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37`.

At the PPC64-21 frontier, the first eight instructions had retired. NIP `0xD328` held `0x7D600164`: primary opcode 31, RS 11, XO 178, L 0, and reserved fields clear. The old MSR, R11, and R12 were all `0x8000000000000000`; CR and XER were zero.

The replay uses the existing Phase 20/21 loader and production VM path: 128 MiB guest RAM, kernel physical range `[0,0x29C2158)`, entry NIP 0, MSR `0x8000000000000000`, GPR3 `0x7FF0000`, GPR4/GPR5 zero, direct FDT, and RTAS disabled. The kernel and FDT were not changed.

## MTMSRD decode and supported behavior

`include/PPC64ISAPlugin.h` defines the supported MSR fields and `DecodedKind::MTMSRD`. `src/isa/PPC64ISAPlugin.cpp` decodes primary 31 / XO 178, extracts RS and L, checks RA, the low RB fields, and Rc, and disassembles both valid forms. Invalid reserved fields remain an invalid MTMSRD form and raise the existing program exception.

The POWER9 Book3S model's L=0 writable mask is independently asserted in the test as `0x800000000000EF32`: SF, EE, PR, FP, FE0, SE, BE, FE1, IR, DR, and RI. HV, S, ME, LE, and unmodeled/reserved bits are excluded. L=1 uses the independently qualified mask `0x8002`, so it changes only EE and RI. GPR0 is read as the architectural source register.

Guest code cannot acquire or remove HV authority with this instruction. The implementation preserves current S, ME, and LE. Bits rejected by the profile are not copied from the source. SF is represented, but a change that would enter 32-bit effective-address mode returns `UNSUPPORTED_STATE` without changing MSR because that execution mode is not implemented. IP and LE remain invalid in the active profile.

MTMSRD requires privileged execution. Problem-state execution enters the existing Program exception path with the privilege cause in SRR1 and no partial MSR write. Invalid instruction fields use the illegal-instruction cause. A valid configured hypervisor context was also exercised. For L=0, entering problem state enforces EE, IR, and DR; the supported S:HV:PR=`110` case clears IR and DR. The test checks fault vector, saved NIP/MSR context, and source-register preservation.

Changes commit through the existing `setMSR()` state mechanism. Changed MSR values invalidate the cached decoded instruction; relocation changes clear reservations. The VM evaluates pending interrupt eligibility at the next instruction boundary. The guest's separate `isync` remains a separate instruction.

## Translation and interrupt qualification

Configured POWER9 radix tests execute guest instructions across instruction relocation changes and perform a subsequent translated data load after a data relocation change. The PR transition case changes IR and DR together and verifies the next fetch uses the new state. Missing radix configuration and unsupported SF mode transitions are rejected without partial state changes. These tests qualify the implemented 64-bit radix profile, not every theoretical MSR combination.

The production VM XICS test starts with EE enabled and uses L=1 to disable it. A pending XICS interrupt then remains masked while EE is clear. L=1 re-enables EE and retires; delivery occurs at the following boundary. The guest handler reads XIRR, acknowledges and completes the source, and returns with `rfid`. SRR0/SRR1 and virtual-time behavior are checked. A deterministic DEC countdown underflows while EE is disabled; after L=1 enables EE, the DEC interrupt is delivered at the next boundary without charging virtual time for the exception-entry boundary.

Two separate `VirtualMachine` instances were exercised: MTMSRD changes the first VM's MSR and NIP while the second VM's MSR, NIP, and GPR remain unchanged. The existing Phase 7 suite also re-ran its two-LPID radix and backing-isolation coverage. No CPU-context banking or SMP behavior was added.

The v5 PPC64 CPU state round-trip preserves the post-MTMSRD MSR and NIP, and the restored CPU retires the following instruction. The existing VM checkpoint and Phase 20/21 suites also passed, preserving their checkpoint compatibility coverage; the writer format was not changed.

## Focused and replay results

`test_ppc64_phase22` passes **278 checks, zero failures**. Its coverage includes exact Linux decode, both L forms, independently derived writable masks, reserved encodings, privilege exceptions, protected state, no-partial-fault behavior, translation changes, two-VM state isolation, v5 restore continuation, XICS and DEC delivery, and production Linux replay.

Two fresh VM lifecycles loaded the same kernel, used identical ELF load plans, FDT bytes, entry registers, virtual time, and instruction budget. The trace, FDT, memory plan, entry registers, retired/attempted counts, and final frontier were identical.

At NIP `0xD328`, `mtmsrd r11` retired normally. MSR remained `0x8000000000000000`; the next NIP was `0xD32C`; CR and XER were unchanged. The following `isync` and control-flow instructions ran. After 27 retired instructions and 28 attempts, execution stopped reproducibly at NIP `0xCFF4` on `0x7C1F42A6`, decoded as unsupported `mfspr r0,287` (PVR). This is bounded instruction execution through the early Linux path, not a Linux boot.

The exact replay evidence and its checksum inventory are under:

`E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase22\final\run-1793638807731\`

The trace SHA-256 is `59995d49b468dd7941f837600930625ceee0f16df5926a5c20fdf607af35e962`. The frontier summary SHA-256 is `9387d1d5aafbae2287439847f1bb774acfe3e043dae50f418846672e536ea216`. The copied FDT SHA-256 matches the original: `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37`. The per-run manifest SHA-256 is `0280aa20178e47a1ea049455851af3c7f81ce5f6decff8237e30f3e6b18fa674`.

The preserved Phase 21 trace and frontier summary were re-hashed in place and still match `8a163a8df887f7953c9d0b6e1fe3d22a3ade3d0d5b9df8131e12afc3daf54371` and `651e9778e6fb8fe0a0202446c28638fcb6c3584f6f8565710b62c84cb9789962`, respectively.

## Programs A–N

| Program | Result |
|---|---|
| A — Exact instruction decode | Pass |
| B — General L=0 semantics | Pass |
| C — Limited L=1 semantics | Pass |
| D — Protected MSR fields | Pass |
| E — Privilege enforcement | Pass |
| F — Architectural side effects | Pass |
| G — Supported IR/DR transitions | Pass; unsupported modes fail safely |
| H — XICS external interrupt behavior | Pass |
| I — DEC timer interrupt behavior | Pass |
| J — VM and LPID isolation | Pass; two VMs plus the existing two-LPID Phase 7 isolation suite |
| K — Checkpoint compatibility | Pass; v5 state restore and continuation plus existing checkpoint suites |
| L — Authentic Linux replay | Pass; original eight instructions reproduced |
| M — Cross `0xD328` blocker | Pass; MTMSRD retires and advances to `0xD32C` |
| N — New frontier and regression | Pass; two identical replays and previous PPC64 suites pass |

## Regression and build status

The full configured CTest run has 36 entries: **30 passed, zero executed failures, and 6 Not Run** because the Syscall, Debugger, Profiler, Fuzzer, VMIsolation, and ELFValidation executables are absent. This matches the PPC64-21 configured baseline; all PPC64 Phase 1–22 CTest suites ran and passed. All previous **12,091 PPC64 checks** therefore remain passing. Shared Basic, NewInstruction, EFI memory map, IA-64 SAL, VM checkpoint, RISC-V decoder smoke, and both PPC64 console tests passed.

Standalone IA-64 ACPI, interrupt-controller, and Processor Interrupt Block tests each passed. The full `cmake --build build -j 4` still encounters unrelated legacy link failures in `test_watchpoint`, `test_mov_imm`, `test_cpu_execution`, and `debug_harness` (missing shared IA-64/PPC64 symbols and/or MinGW Winsock imports). The Phase 22 target builds successfully, and no PPC64 Phase 1–22 test target failed to build or execute. The unrelated build infrastructure was left unchanged.

## References and next scope

The normative behavior was checked against the [OpenPOWER ISA specifications](https://openpowerfoundation.org/specifications/isa/). The source-level compatibility comparison used the pinned QEMU [`MTMSRD` decode](https://github.com/qemu/qemu/blob/56afd82bd4db9688f1c1c9d0e2ea163b8cde3283/target/ppc/insn32.decode), [translation](https://github.com/qemu/qemu/blob/56afd82bd4db9688f1c1c9d0e2ea163b8cde3283/target/ppc/translate/misc-impl.c.inc), and [MSR helper](https://github.com/qemu/qemu/blob/56afd82bd4db9688f1c1c9d0e2ea163b8cde3283/target/ppc/helper_regs.c). Linux comparison used the [v7.2 64-bit early entry](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/head_64.S) and [MSR definitions](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/include/asm/reg.h). QEMU runtime was not run; `QEMU_REFERENCE_PENDING` remains the external runtime status. No QAC was launched.

The exact next bounded scope is PPC64-23: implement guest-readable PVR SPR 287 (`mfspr`) and replay this same immutable kernel to identify the next frontier. Do not add unrelated instructions or platform services.
