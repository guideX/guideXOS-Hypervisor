# IA-64 POST-MADT Phase 9 — Scheduler Context and Atomic Integrity

**Outcome C — diagnostic root cause isolated; safe architectural repair not ready.**
The first scheduler divergence is an emulator frame-snapshot ownership error.
The suspected stale atomic read is disproved: an ordinary guest FP spill wrote
the value returned by the atomic operation. No production semantics or guest
artifacts were changed. `.phase` remains at Phase 8 → Phase 9.

## 1. Git state and preserved inputs

- Starting branch/HEAD: `IA64` / `fd7b6fff336bba89fc2e3fa39533f1ed1f6ff5b6`.
- Starting phase: `last_completed=IA64-POST-MADT-8`,
  `next_expected=IA64-POST-MADT-9`; upstream divergence `0 0`.
- The worktree contained temporary Phase 9 trace changes in
  `tools/ia64_iso_matrix.cpp` and a temporary frame logger in
  `src/isa/IA64ISAPlugin.cpp`. Both were removed after collecting the trace.
- Ending branch/HEAD and phase are unchanged. No commit or push was made.
  The report is new; the pre-existing untracked `build-7r1/` remains untouched.
- No branch, reset, clean, stash, rebase, merge, or worktree operation was
  performed. No guest image or checkpoint was written.

Final SHA-256 verification in the sibling `.codex_tmp` directory:

| Artifact | Size | SHA-256 |
|---|---:|---|
| `bootia64_exact.efi` | 378,664 | `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA` |
| `authentic-linux-decompressed.bin` | 13,035,024 | `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483` |
| `post_mux1_fixed_ebs.chk` | 574,577,483 | `71BDE2056B4FCA11007B850CA49897FB7D926E5BA2AC220760A64FF8A94ECA8B` |

## 2. Baseline and replay

The accepted read-only checkpoint is `post_mux1_fixed_ebs.chk` at VM cycle
`785426373`. Phase 8’s independent 100M/250M continuations and guest RAM
inspection remain the baseline: `start_kernel`, `rest_init`, `kernel_init`,
`kthreadd`, `smp_prepare_cpus`, and `do_pre_smp_initcalls` are reached;
`kernel_init` waits for `kthreadd`, which eventually stalls in a ticket-lock
path. No `init_post`, root mount, or userspace entry was verified.

I restored the original checkpoint read-only and repeated a bounded
50,520,000-step continuation with scheduler and frame-owner tracing. It ended
at VM cycle `835946373`, after the preemption-accounting transition at cycle
`835936154`. The task A/B stores and the cross-task frame selection were
reproduced. The longer 139M-step atomic-history replay also completed without
an emulator atomic-path discrepancy; it remains at the same guest frontier.

## 3. First chronological Linux diagnostic

The first extracted `BUG:` is at guest time `t=0.131881`:

```text
BUG: scheduling while atomic: kthreadd/2/0xc0000000
```

It is emitted by Linux 3.2.78-1 `schedule_debug()` / `__schedule_bug()` in
`kernel/sched.c`. It is a nonfatal scheduler diagnostic, not a fatal Linux
`BUG()` stop: the guest continues and reports the same condition about 323
times. It follows the bad task-local `PREEMPT_ACTIVE` subtraction and precedes
the later ticket-lock memory overwrite. The guest’s IA-64 definition puts
`PREEMPT_ACTIVE` at bit 30 in `thread_info.preempt_count`.

## 4. Task A/B timeline and first emulator divergence

Task A is the init/`swapper/0` task (`r13=0xe000000008ec0000`); task B is
`kthreadd` (`r13=0xe000000008ec8000`). The Linux `__paravirt_switch_to`
assembly saves the outgoing stack pointer, loads the incoming stack pointer,
sets `r13` to the next task, and returns in that context. The `r13` change is
therefore expected. See Debian’s [Linux 3.2.78-1 IA-64 switch code](https://sources.debian.org/src/linux/3.2.78-1/arch/ia64/kernel/entry.S/).

| VM cycle | Context and event |
|---:|---|
| 835910662 | Idle/boot context switches to B in `__paravirt_switch_to`; B gets its own stack and `r13`. |
| 835914204 | B switches to A; this is a normal task switch. |
| 835934981 | A executes `preempt_schedule`’s `st4` at `0x7c050`, adding `0x40000000` at PA `0x8ec0c98`. A’s word changes `0 → 0x40000000`. |
| 835935976 | A switches to B. The change `r13=A → B` is the kernel’s expected current-task update. |
| ≈835936070 | On B’s resumed return through `__paravirt_switch_to`, the emulator’s frame helper matches A’s newer snapshot for return target `0x4824f30`, despite an older matching B snapshot. |
| ≈835936154 | B reaches the `preempt_schedule` exit store at `0x7c080`; its word at PA `0x8ec8c98` was zero, so subtracting `0x40000000` writes `0xc0000000`. |

The frame-owner trace directly exposes the first incorrect emulator
transition. These cycle values are the guest AR44 clock; the paired E1
instruction trace records absolute VM cycles and shows the corresponding
`0x7c080` store at 835936154.

| Frame event | Owner at save | Return target | Stack pointer | Result |
|---|---|---|---|---|
| Save at AR44 47,617,094 | B | `0x4824f30` | `0xe000000008ecfe00` | B’s older frame remains at global stack index 11. |
| Save at AR44 47,637,984 | A | `0x407c060` | `0xe000000008ec7d70` | A’s `preempt_schedule` frame, CFM `0x204`. |
| Save at AR44 47,638,866 | A | `0x4824f30` | `0xe000000008ec7d60` | A’s newer frame at global index 22. |
| Restore at AR44 47,639,064 | **B** | `0x4824f30` | `0xe000000008ecfe00` | Reverse search selects A’s index 22, not B’s index 11. |
| Restore at AR44 47,639,148 | **B** | `0x407c060` | `0xe000000008ecfe10` | After the first erase, reverse search selects A’s index 21 (`CFM=0x204`). |

`CallFrameSnapshot` stores stacked GRs, CFM, and return address, but has no
context owner. `saveCallFrame()` pushes into one global `callFrameStack_`;
`restoreCallFrame()` reverse-searches only by return address and erases the
matching frame and every newer entry. The completed-frame map is also keyed
only by return address. Thus B’s first return consumes A’s later frame and
erases interleaved entries; B’s next return consumes A’s pending
`preempt_schedule` frame. At the latter return, the saved A register context
is applied while B is active, immediately before B executes the unmatched
subtract. The per-task word trace confirms A remains at `0x40000000` while B
becomes `0xc0000000`.

This isolates the first incorrect emulator transition at the
`restoreCallFrame()` cross-context match for `0x4824f30`, before the bad
`0x7c080` store. It is not an error to switch `r13`, nor a Linux accounting
rule that permits subtracting A’s `PREEMPT_ACTIVE` from B’s word.

### Why the defect is diagnosed but not patched here

The fallback snapshot has no stable architectural context key. `r13` is
Linux’s current-task convention, not an IA-64 context identifier. A raw
`BSPSTORE` key is also insufficient: Linux’s switch-stack path performs
`flushrs` and restores `BSPSTORE`, so its value advances within a task across
a context switch. A stack-pointer key would separate this Linux trace, but
would be an unproven substitute for RSE backing-store identity across address
spaces and nonlocal returns. A safe fix needs an explicit RSE/call-frame
context model, including checkpoint compatibility, and a focused test that
proves two contexts with the same target cannot consume or erase each other’s
frames. No guest-specific `r13` rule or hardcoded task/address workaround was
added.

## 5. Atomic read and ticket-lock chronology

The Linux IA-64 ticket lock uses `TICKET_SHIFT=17` and `TICKET_MASK=0x7fff`
in the guest’s `arch/ia64/include/asm/spinlock.h`. At cycle `923620015`, the
raw 16-byte bundle at `0x829830` is:

```text
0b 00 00 02 07 00 30 18 80 22 21 00 00 00 04 00
```

Its MMI operations include `rsm` and `fetchadd4.acq r3=[r32],1`. The atomic
is a 4-byte acquire read-modify-write with increment 1. At guest PA
`0x8ec7dd8`, the physical read hook sees the current little-endian bytes
`3e 00 01 00` (`0x1003e`); the operation returns that old value and writes
`3f 00 01 00` (`0x1003f`). The load and update address the same RAM backing
store. Translation, width, byte order, hook coverage, and atomic write
consistency all agree.

The missing write in Phase 8’s reconstruction is at cycle `923613750`, 6,265
cycles earlier. Task A (`r13=0xe000000008ec0000`) executes the valid
`stf.spill [r3]=f11` at `0x8a40` slot 1, with `r3=0xe000000008ec7dd0`. The
architectural 16-byte spill writes
`00000000000000003e00010000000000` to PA `0x8ec7dd0`, replacing the word at
`0x8ec7dd8` with `0x1003e`. The later atomic read is therefore correct. The
report’s prior “stale fetchadd” conclusion is withdrawn.

The history contains 52,552 balanced takes/unlocks before the reported bad
fetchadd. The pointer used by B (`r32`) resolves to `0x8ec7dd8`, inside A’s
task stack; the 16-byte guest spill altered that word before the fetchadd.
This is consistent with the earlier frame leak but the register provenance
of the lock pointer was not captured end-to-end. Therefore the scheduler
defect is proven; a causal link from it to the lock pointer is plausible but
not claimed as proven. IRQs disabled in the ticket slow path do not indicate
an atomic emulator defect.

## 6. Relationship between findings

- **Scheduler / `PREEMPT_ACTIVE`:** proven emulator defect. The global frame
  snapshot stack selects A’s frame when B resumes the same return target.
- **Atomic fetchadd:** no defect established. The value comes from the earlier
  guest FP spill, and the atomic read/write path is consistent.
- **Ticket-lock stall:** its memory word was overwritten by guest execution at
  an address in A’s stack. The scheduler frame leak may explain why B used
  that pointer, but this replay did not prove the pointer’s register
  provenance. The issue is not described as stale atomic state.
- **Causal classification:** H1 is proven for preemption accounting and
  remains a hypothesis for the later lock pointer; H2 and an atomic-memory
  version of H3 are rejected; H4 describes the independently valid atomic
  operation; H5/H6 remain open only for the lock-pointer connection.

## 7. Tests and continuation status

The isolated MinGW/Ninja build ran these suites successfully before the
diagnostic-only frame logger was removed:

- `test_new_instructions` — all tests passed.
- `test_ia64_processor_interrupt_block` — passed.
- `test_ia64_interrupt_controller` — passed.
- `test_vm_checkpoint` — passed.

These suites do not cover interleaved task-owned call-frame snapshots, so
they are not a regression test for this defect. No targeted test or repair
was added. `test_isa_plugin` was not run because the established run exceeds
300 seconds without progress; `test_cpu_execution` was not run because of
the known MinGW `ws2_32` link failure. Phase 7R1’s accepted RCU behavior
remains the baseline; the Phase 8 100M replay reported zero RCU faults. The
Phase 9 bounded continuation serviced timer interrupts (35 entries in the
50.52M-step replay) and showed no new RCU fault.

No repaired continuation was possible because no safe fix was applied. The
highest verified Linux milestone is unchanged: `kernel_init` is parked in
`wait_for_completion`, while `kthreadd` runs into the scheduler/ticket-lock
frontier. No `init_post`, root mount, or userspace process launch was
observed. Phase 8’s bounded run reported 323 nonfatal atomic-scheduling
diagnostics; the Phase 9 trace identifies the emulator mechanism behind the
first observed `PREEMPT_ACTIVE` underflow but does not claim a post-repair
count.

## 8. Recommended Phase 10 work

1. Define a stable RSE/call-frame context identity that follows task
   suspension and resumption, including `flushrs`, `loadrs`, `cover`, and
   nonlocal returns; do not use Linux `r13` as an architectural identity.
2. Add a deterministic regression with two contexts saving the same return
   address. Verify each context restores its own CFM/stacked registers and
   that unwinding one context does not erase another’s frames. Version and
   retain compatibility for serialized checkpoint state.
3. Re-run the four passing suites plus relevant RSE/interrupt tests, then
   restore the original checkpoint and demonstrate the corrected per-task
   `preempt_count` through the previous 250M/139M frontier.
4. Trace the source register for B’s lock pointer `0x8ec7dd8`; determine
   whether that stack address is a second effect of the same frame leak.
   Do not change fetchadd semantics or modify guest Linux.

**Closeout:** Outcome C; `.phase` remains 8 → 9; ending HEAD remains
`fd7b6fff336bba89fc2e3fa39533f1ed1f6ff5b6`; report uncommitted; push not
attempted. The final worktree contains this report and the preserved
pre-existing untracked `build-7r1/` only.
