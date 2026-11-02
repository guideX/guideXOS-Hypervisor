# IA64-POST-MADT-9R20 — `driver_register` r14 Provenance

## 1. Outcome

**Outcome B — the first incorrect architectural transition is proven and corrected; authentic continuation beyond it remains incomplete.**

The guest’s `r14=0x4000` was written by ordinary Linux instructions in the `printk` path. That value is a legal scratch-register clobber. Linux expected its preceding `BUG()` to execute `break.m 0`, which does not return. The emulator decoded the M-unit break as a NOP, returned from `printk`, and let `driver_register` use the clobbered `r14` to form `0x4040`. This is the first incorrect transition.

The decoder and execution path now recognize M37 `break.m` and deliver the Break Instruction fault at IVA+`0x2c00`. A focused regression exercises the production decoder and fault delivery. The post-fix Linux replay did not reach `driver_register`: it emitted `Attempted to kill the idle task!` earlier in the continuation. R20 therefore does not claim that Linux crossed the original failure or reached a new boot milestone.

## 2. Repository, branch, and phase state

- Repository: `D:\dev\guideXOS_Hypervisor_IA64`
- Started on branch `IA64`, HEAD `9861576b7dc7cd826a88bf9c16044cc11a8dddb9`, subject `docs(ia64): record R19 closeout commit`.
- Starting worktree was clean; `HEAD...origin/IA64` was `0 0`.
- `.phase` remains `last_completed=IA64-POST-MADT-8` and `next_expected=IA64-POST-MADT-9`.
- The final local commit SHA and final worktree state are recorded in §28–29. No push was made.
- R20 build output and traces are under the ignored `build-9r20` directory. Historical build and evidence directories were retained.

## 3. R19 baseline verified

R19’s [init-panic provenance report](IA64_POST_MADT_9R19_INIT_PANIC_PROVENANCE.md) is preserved. It established the fault-to-panic chain for the same Linux artifacts and R16 checkpoint:

- `driver_register` entry: canonical IP `0xa000000100580ea0`.
- Faulting instruction: `driver_register+0x90`, canonical IP `0xa000000100580f30`, slot 1, `ld8 r3=[r8]`.
- Immediately preceding address calculation: `driver_register+0x80`, `add r8=r14,64`, with `r14=0x4000`, producing `r8=0x4040`.
- The 8-byte load faulted at `0x4040`; saved CR.IFA was `0x4040`, CR.ISR was `0x400000000` (load access), and Linux entered `ia64_do_page_fault`, `die()`, then `do_exit(SIGSEGV)`.
- The exiting task pointer `r13=0xe000000008ec0000` was proven to equal `init_pid_ns.child_reaper`; `find_new_reaper()` took the init reaper panic branch. No numeric PID/TGID is asserted.

R19 did not identify the writer of `r14`; R20 supplies that missing provenance. The timer RFI recorded later in R19’s `die()` continuation is separate from the pre-fault `driver_register` interval.

## 4. Guest artifacts and replay identity

R20 reused the current-format R16 checkpoint. Its manifest says format version 1, architecture IA-64, checkpoint kind `after-ExitBootServices-kernel-handoff`, build identity `guideXOS-Hypervisor/IA64-checkpoint-v1`, and checkpoint cycle `778197459` (ITC `0x2e6259d3`).

| Artifact | SHA-256 |
|---|---|
| `build-9r16/r16-authentic-ehandoff.gxcp` | `B6C258887359E4E6A5B0BCFE355B86FD6CFC7A410D735F575BE6688348564B4B` |
| checkpoint manifest | `3B030AEA30F29C1C3518F7401B49C95802521F6AB183ED753CED14848990ACE6` |
| `build-9r9/authentic-reconstructed.iso` | `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E` |
| `LINUX.gz` | `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C` |
| `INITRD.GZ` | `D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B` |

The trace-on/off scripts use the same 512 MiB guest, ISO, Linux/initrd identity string, checkpoint, and 59,130,641 post-checkpoint cycle budget. They are preserved as `build-9r20/evidence/r20-r14-trace-{on,off}-v3.ps1`.

## 5. Authentic `driver_register` instruction sequence

The authoritative sequence is the Debian Linux 3.2.78-1 guest binary, corroborated by its dynamic trace. Bundle values below are the captured low/high 64-bit halves; the raw instruction is the 41-bit slot value.

| Offset | Slot and raw instruction | Decode / effect |
|---|---|---|
| `+0x00` | slot 0, `0x2c00a1a9c0` | `alloc r39=ar.pfs,13,10,0` |
| `+0x06` | slot 1, `0x10802010380` | `add r14=r32,8` |
| `+0x10` | slot 0, `0x80c0e00380` | `ld8 r14=[r14]` |
| `+0x16` | slot 1, `0x188000980` | `mov r38=b0` |
| `+0x1c` | slot 2, `0x10800ef0080` | `add r2=r14,120` |
| `+0x26` | slot 1, `0x80c02003c0` | `ld8 r15=[r2]` |
| `+0x30` | slot 1, `0x1c830f001c0` | `cmp.eq p7,p6=0,r15` |
| `+0x3c` | slot 2, `0x800000a006` | `br.cond` to `+0x80`, qualified by `p6` |
| `+0x60` | slot 2, `0xa053427000` | `br.call b0` to canonical `0xa00000010081b030` (`printk` path) |
| `+0x70` | slot 0, `0x0` | architectural `break.m 0`; pre-fix emulator reported `nop` |
| `+0x80` | slot 0, `0x10800e80200` | `add r8=r14,64` |
| `+0x8c` | slot 2, `0x10802060280` | `add r10=r32,48` |
| `+0x90` | slot 1, `0x80c08000c0` | `ld8 r3=[r8]` — the R19 fault |

Captured bundles include entry `0x40e0058014353809 / 0xca080520420080`; `+0x10` `0x26010181c007003 / 0x8400778040006200`; guard/branch `+0x30` `0x70000100000010 / 0x4000005003720c3c`; break `+0x70` `0x8 / 0x4000000000200`; address calculation `+0x80` `0x21001d004009 / 0x8401030140000200`; and fault load `+0x90` `0x30000100000009 / 0x4000000203020`.

The guard comparison is true for equality (`p7`) when `r15==0`; the complementary `p6` branch to `+0x80` is then false. Linux falls through the `BUG_ON` path, calls `printk`, and places `break.m 0` at its return address. If the break fault is correctly delivered, it does not return to `+0x80`.

## 6. Dynamic invocation identity

The bounded pre-fix trace covers guest cycles `[837298900,837328100)`. The target invocation begins at cycle `837299010` in bundle `0xa000000100580ea0`, slot 0. At entry, `r32=0xa000000100c6d300` is the driver pointer, `r13=0xe000000008ec0000`, and `b0=0xa00000010048d220`. The first instruction allocates the function frame; the next instruction computes from the live `r32` argument.

The `br.call` at `+0x60` enters the `printk` call chain. Its nested calls return, then `br.ret b0` at cycle `837327807`, canonical IP `0xa00000010081b0d0`, returns to the same invocation’s `+0x70` continuation. `r13` and `r32` still identify the same task and driver. The next bundle is at canonical IP `0xa000000100580f20`, followed by the faulting load at `+0x90`. This is one function invocation and one task, not a cross-call or cross-task register comparison.

## 7. Faulting load

At cycle `837327815`, slot 1 executes raw `0x80c08000c0`, `ld8 r3=[r8]`, with `r8=0x4040`. It is an 8-byte load at `driver_register+0x90`; it faults before committing a destination value. R19 recorded the resulting CR.IFA, CR.ISR, interruption entry, Linux page-fault handler, exit, and init-reaper panic. The exception is not suppressed by R20.

## 8. Initial r14 and driver/bus pointer provenance

At the trace window’s first cycle, `r14` is already `0x4000` in unrelated code. That is outside the target function and is not the provenance answer. At the actual `driver_register` entry bundle, `r14=0xa000000100c6d310`; then slot 1 executes `add r14=r32,8`, with `r32=0xa000000100c6d300`, producing `0xa000000100c6d308`.

An 8-byte store at cycle `837299005` writes `0xa000000100c6ce30` to physical `0x4c6d308`. At cycle `837299013`, the function’s `ld8 r14=[r14]` reads bytes `30 ce c6 00 01 00 00 a0` from that physical address and loads `0xa000000100c6ce30` into `r14`. This is a valid kernel pointer to the bus object. The trace’s DTR translation details and exact read bytes are in `r20-r14-provenance.log`.

## 9. Relevant writes to r14

The complete bounded trace records 29,200 guest steps and 1,911 r14 candidate events. Events record instruction IP, bundle/slot, raw slot, decoded operation, predicate and result, before/after values and NaTs, source operands, CFM/PFS, PSR, RSC, BSP/BSPSTORE/RNAT, and context registers. Predicated-off candidates are distinguished from committed transitions.

Within this one invocation, the value `0x4000` is written by ordinary immediate-add instructions at these cycles:

| Cycle | Canonical IP / slot | Before → after |
|---:|---|---|
| 837302577 | `0xa0000001000d3b80`, 0 | `0x0ecd35a2 → 0x4000` |
| 837311235 | `0xa000000100829910`, 2 | `0xa000000100c54d4c → 0x4000` |
| 837313402 | `0xa000000100829910`, 2 | `0xe000000000000000 → 0x4000` |
| 837313847 | `0xa000000100829910`, 2 | `0xe000000000000000 → 0x4000` |
| 837324387 | `0xa000000100829910`, 2 | `0xe000000000000000 → 0x4000` |
| 837326419 | `0xa000000100829910`, 2 | `0xe000000000000000 → 0x4000` |
| 837326903 | `0xa000000100829910`, 2 | `0xe000000000000000 → 0x4000` |
| 837327375 | `0xa000000100829910`, 2 | `0xe000000000000000 → 0x4000` |
| 837327675 | `0xa000000100829910`, 2 | `0xa000000100c54d4c → 0x4000` |
| 837327735 | `0xa000000100829910`, 2 | `0x0 → 0x4000` |
| **837327778** | **`0xa00000010008b460`, 0** | **`0xa000000100c80ac8 → 0x4000`** |

Every listed write decodes as `addl r14=0x4000,r0`, with `p0` true, `r0=0`, no NaT, and immediate `0x4000`. The first post-entry write is at cycle `837302577`; later helper code overwrites it. The decisive last write is cycle `837327778`, inside the final `printk` return chain. It remains in `r14` through `br.ret` at cycle `837327807` and into the expected non-returning break at `+0x70`.

## 10. Predicate, slot, and bundle evidence

At the decisive write, raw slot `0x12400000380` is an M-unit immediate add in slot 0, predicate field 0 (`p0`), predicate true. At the fault site, `break.m 0` is raw slot zero in M slot 0 of bundle `0x8 / 0x4000000000200`; the pre-fix decoder labeled it NOP and advanced. The subsequent `+0x80` add and `+0x90` load are each predicated true. There is no predicated-off write or canceled load that explains the observed value.

## 11. Source register and memory evidence

The decisive `addl` reads architectural `r0`, which is zero and not NaT, and adds the zero-extended immediate `0x4000`; the expected result is exactly `0x4000`. The emulator result matches IA-64 arithmetic. The value is not read from a corrupted pointer field.

The guard load at `+0x26` targets the bus pointer plus 120 and loads zero into `r15`; the following executed compare and complementary-predicate branch establish that result. The memory watch was set on the corresponding physical address `0x4c6cea8` and reported no writes during the bounded interval. This tracer serializes load bytes for loads whose destination is `r14`, so it did not separately serialize the raw bytes of the `r15` guard load. The architectural loaded value is established by the live compare/branch, not by a captured byte dump.

## 12. Calls, returns, and r14 lifetime

The relevant driver call is the `br.call` at `+0x60` into the `printk` path. The final nested helper sequence includes a `br.call` at cycle `837327759`, a `br.ret` at `837327768`, another helper return at `837327777`, the decisive `addl` at `837327778`, a nested return at `837327798`, and the `printk` return to `driver_register+0x70` at `837327807`.

IA-64 `r14` is a static general register, not an RSE-stacked register. Under the IA-64 calling convention it is caller-scratch. Linux may leave the scratch value in place because `BUG()` is expected not to return. No save or restore of the pointer in `r14` is required across this call when the following break faults. The helper’s legitimate clobber is not itself an emulator error.

## 13. Interruptions and RFI

No `rfi` instruction or interruption-vector execution was captured in the bounded interval from the target function entry through the faulting load. CFM/PFS changes are associated with the nested call frames; no checkpoint restore, RSE rename of static `r14`, or task switch explains the value. The R19 timer RFI occurs later, during the Linux `die()` continuation after the original load fault, and remains separate evidence.

## 14. Guest object and field analysis

The driver pointer is `0xa000000100c6d300`; the emitted `+8` address accesses its bus pointer and the live memory value is `0xa000000100c6ce30`. The machine code then loads the guard at bus-object offset `+120`, and the runtime value is zero. The matching Linux 3.2.78-1 `drivers/base/driver.c` source at line 227 contains `BUG_ON(!drv->bus->p)`.

There is a source/binary layout discrepancy: the local Debian source header’s visible `struct bus_type` declaration places `p` at offset 104, while the authentic binary computes `bus+120`. R20 therefore reports the observed binary access as raw offset `+120` and does not claim that source layout alone proves this exact location is `p`. The zero guard result and the compiled `BUG_ON` path are proven; exact C-field reconciliation remains open.

## 15. Earliest incorrect transition

The first incorrect transition is at cycle `837327808`: the emulator recognizes raw M37 slot `0x0` at `driver_register+0x70` as a NOP and retires it. IA-64 defines that encoding as `break.m 0`; it must raise a Break Instruction fault and not return to the following bundle. The first subsequent incorrect guest continuation is `add r8=r14,64` at cycle `837327811`, using the caller-scratch value left by `printk`. The resulting load faults at `0x4040` at cycle `837327815`.

The immediate writer at cycle `837327778` is a legitimate guest operation. The architectural mismatch is the missing break fault, not the arithmetic, `r14` scratch lifetime, the pointer load, or the later data-fault delivery.

## 16. Guest versus emulator classification

The write of `0x4000` is authentic guest execution from a real `addl` instruction. It is correct for its source and immediate. Linux’s emitted `BUG()` depends on the break instruction being non-returning. Treating the break as NOP makes the guest execute code the compiler and kernel source do not expect to execute. That is a proven emulator semantic defect, not evidence of RSE corruption or a bad caller-saved register implementation.

## 17. Diagnostic instrumentation

`tools/ia64_iso_matrix.cpp` now offers `--r14-provenance START END PATH` and repeatable `--r14-watch-memory-pa PA` options. They are disabled by default, limit output to a caller-selected cycle window, and record instruction/pre-post state plus watched memory writes and r14-destination load details. Trace-on/off replay state dumps were compared to check instrumentation neutrality.

The complete pre-repair trace is `build-9r20/evidence/r20-r14-provenance.log` (19,967,574 bytes, SHA-256 `A2E1F5EF375D8DF8E32C92972AFC413A6EB78DAD2B93886CC306A634688D60F8`). The post-repair trace is `r20-r14-provenance-v3.log` (19,656,344 bytes, SHA-256 `D7CF86633724D36CE9C0E88C3538CAD7AC1919C688CFF6F0E6B524A52354C0EE`). Both preserve requested-window summaries.

Evidence integrity note: an initial post-fix tracer launch accidentally used the v2 provenance-log path and truncated `r20-r14-provenance-v2.log` to zero bytes before it was stopped. That empty file is not used as evidence. The complete pre-fix v1 trace above and the complete post-fix v3 trace are preserved; the v2 console, progress, and memory-dump artifacts were not overwritten.

## 18. Production repair

The production change is limited to M37 decode and Break Instruction fault delivery:

- `MTypeDecoder` recognizes the exact M37 fixed fields and reconstructs the dispersed 21-bit immediate.
- The M-unit NOP fast path now matches `x6==1`; it no longer swallows `x6==0` M37 breaks.
- M37 is represented as `InstructionType::BREAK`. With `PSR.ic` set and the predicate true, non-syscall breaks save the faulting bundle in IIP, the interrupted PSR/slot in IPSR, slot information in ISR.ei, the immediate in IIM, clear IFS.v and interruption-entry PSR state, and enter IVA+`0x2c00`.
- The established `break 0x100000` syscall path remains unchanged. Predicated-off breaks and breaks with interruption collection disabled retire without taking this fault.

The regression validates the production decoder/execution path and the state used by the Linux IA-64 break vector. R20 does not add general IIPA modeling; the matching Linux handler path used for this bug does not read IIPA.

References: [Intel Itanium Architecture Software Developer’s Manual, Volume 3](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-vol-3-manual.pdf); [binutils IA-64 M-unit opcode table](https://sources.debian.org/src/binutils/2.43.1-5/opcodes/ia64-opc-m.c); matching Linux source `arch/ia64/kernel/ivt.S` (`break_fault`) and `include/asm-ia64/bug.h` (`BUG()` / `break.m 0`).

## 19. Focused regression

`test_isa_plugin breakfault` passes. The new test decodes a dispersed 21-bit M37 immediate, executes raw `break.m 0` in an MMI-stop bundle, checks IVA+`0x2c00`, IIP/IPSR/ISR/IIM, PSR/IFS state, and verifies predicated-off and `PSR.ic`-clear retirement. It uses explicit runtime checks rather than `assert` side effects.

The existing final-loop M NOP fixture was corrected to set M0 `x4=1`, the actual `nop.m` encoding. RSE and call-frame fixtures that had used all-zero M slots as NOPs now encode `nop.m`; raw zero is `break.m 0` by definition.

## 20. Full ISA plugin result

`test_isa_plugin` passes in the Release `-O3 -UNDEBUG` build. Focused `breakfault`, `shl`, and `serialization` selectors also pass. This keeps assertions enabled for the Release test configuration.

## 21. Architecture regressions

All requested source-defined targets were rebuilt and run directly from `build-9r20/bin` in Release `-O3 -UNDEBUG` mode:

| Target | Result |
|---|---|
| `test_new_instructions` | PASS |
| `test_ia64_processor_interrupt_block` | PASS |
| `test_ia64_interrupt_controller` | PASS |
| `test_vm_checkpoint` | PASS |
| `test_ia64_single_cpu_context_switch` | PASS |
| `test_ia64_rse_automatic_pressure` | PASS |
| `test_ia64_checkpoint_context_continuation` (R7) | PASS |
| `test_ia64_rse_fault_restart` (R8) | PASS |
| `test_ia64_call_frame_return` (R16–R18 call/cover/RFI path) | PASS |
| `test_efi_memory_map` | PASS |
| `test_isa_plugin` and focused selectors | PASS |
| `git diff --check` | PASS |

Per-target outputs are retained as `build-9r20/evidence/r20-test-*.txt`. There is no separate R19 panic-provenance executable. `test_bootstrap` was not run; R19 records its existing Windows Winsock import-library link limitation, and it is not counted as passing.

## 22. Authentic before/after comparison

The pre-repair trace above reproduces the original `driver_register` path and `0x4040` load. Pre-repair trace-on/off continuation artifacts (`*-v2-*`) ended at cycle `837328100`; their memory-range dumps are both 70,320,128 bytes and have SHA-256 `53413CABE7B62264E99CFFC5909B4AE16B6D3F7444FC3E9BBEDB04932AFA71DC`.

Post-repair v3 trace-on/off continuations also ended at cycle `837328100`, each ran 59,130,641 post-checkpoint cycles with no failed VM step, and their 70,320,128-byte memory dumps match at SHA-256 `7F1585BECAA8A18C9AB2AABEA7271588252B070D75367FF4433276EC5DE0772B`. Both reported guest text `Attempted to kill the idle task!` at ITC `0x2e6b8098` (cycle 778,797,208), before the old `driver_register` window. Their final canonical IP is `0xa000000100039230`.

The host harness reports `state=RUNNING panic=no` at the requested replay limit; the guest panic text proves this is not healthy boot progress. The post-fix replay did not reach `driver_register`, so it does not verify the corrected break in that original dynamic path or prove that Linux crosses the old panic. The cause of this earlier idle-task exit is not established by the bounded R14 window.

## 23. Init-panic disposition

R19’s original `Attempted to kill init!` chain remains the accepted pre-repair baseline. R20 proves that its initiating `0x4040` load was downstream of a missed `break.m 0` fault. R20 does not suppress the data fault or claim a successful init exit. In the corrected replay, the observed guest panic is earlier and says `Attempted to kill the idle task!`; its initiating instruction and relation to the break fix remain for R21.

## 24. Linux milestone

No new Linux milestone was reached. The corrected run did not show completion of this `driver_register`, later initcalls, `rest_init`, `kernel_init`, `kthreadd`, or qualified scheduler activity. The panic loop is not boot progress.

## 25. Scheduler frontier

Phase 9 remains open. R20 does not qualify an authentic Linux task switch or scheduler/context-switch behavior with the revised RSE architecture. The `.phase` gate is deliberately unchanged.

## 26. Recommended R21 scope

Start at the corrected continuation’s earlier `do_exit` / `Attempted to kill the idle task!` path and capture the initiating instruction, task context, and any preceding break-fault vector entry. Reconcile the authentic binary’s bus guard at offset `+120` against the exact build’s C layout/configuration. Then continue the same guest without a bypass and record whether it crosses the old `driver_register` failure. Keep scheduler qualification separate until Linux demonstrates a real, verified task switch.

## 27. Source and architecture references

- Matching guest source: Debian Linux 3.2.78-1 `drivers/base/driver.c`, `driver_register()` and its `BUG_ON(!drv->bus->p)` at line 227; local tree `E:\CodexArtifacts\guideXOS_Hypervisor\src-linux-3.2.78\linux-3.2.78`.
- Matching guest handler: Linux `arch/ia64/kernel/ivt.S`, `break_fault` at IVT offset `0x2c00`; non-syscall break reaches the kernel BUG handler.
- Intel Vol. 3 defines M37 `break.m`, predicate qualification, Break Instruction fault, immediate delivery, and the break vector. The binutils table distinguishes fixed-zero M37 from `nop.m` with M0 `x4=1`.

## 28. Commit SHA

R20 implementation commit SHA: recorded after the local commit is created. The final repository HEAD SHA is also included in the closeout response. No remote operation is part of this phase.

## 29. Tracked and untracked worktree state

At closeout, the intended source, test, and report files are committed on `IA64`. `.phase` is unchanged. `build-9r20` and the historical build/evidence directories are ignored and preserved; they are not staged. The final Git status and upstream divergence are recorded in the closeout response.

## 30. Push status

No push was made. R20 is committed locally only.
