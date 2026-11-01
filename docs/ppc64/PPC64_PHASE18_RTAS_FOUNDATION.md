# PPC64 Phase 18 — RTAS Runtime Services Foundation

**Status:** PPC64-18 complete for the hosted direct-hypercall profile.

**Platform:** one-vCPU POWER9/pSeries foundation with XICS compatibility.
**Qualification:** hosted VM execution through authentic PPC64 instructions; no QEMU/QAC or guest Linux boot was required.

## Supported RTAS model

Phase 18 provides a bounded runtime-services dispatcher owned by each PPC64 VM. The guest invokes it with the PAPR `H_RTAS` hypercall. Four registered services configure the production Phase 13 XICS controller. Service callables are host runtime state and are not serialized.

This is the **hosted direct-H_RTAS profile**. It does not implement SLOF, Open Firmware, or a guest-side RTAS forwarding stub. The guest starts through the Phase 10 direct-FDT convention and may issue `H_RTAS` from supervisor state after entry.

## Hypercall ABI and result layers

The guest executes authentic `sc 1` with:

| Register | Input/output |
|---|---|
| GPR3 | Input `0xF000` (`H_RTAS`); output PAPR hypercall status |
| GPR4 | Guest-real address of the RTAS parameter block |

The hypervisor leaves service results in the guest block. A registered token returns outer `H_SUCCESS`, including when its service ABI or source arguments are invalid; return cell zero then contains RTAS `-3`. An unknown or unadvertised token writes RTAS `-3` to return cell zero and returns outer `H_PARAMETER`. Invalid pointers, malformed headers, impossible counts, or an unavailable response return `H_PARAMETER` without dispatch. A registered token with a bad argument/return count is recognized and returns `H_SUCCESS` with RTAS `-3` when a status cell is available.

`H_RTAS` is dispatched only from the existing `sc 1` path. It does not use `sc 0` and does not bypass guest instruction execution. Existing terminal and XICS PAPR services remain in the same dispatcher.

## Parameter-block wire format

All cells are big-endian 32-bit values. The wire block contains no host-side `rets` pointer.

| Offset | Size | Field |
|---:|---:|---|
| `0x00` | 4 | Service token |
| `0x04` | 4 | `nargs` |
| `0x08` | 4 | `nret` |
| `0x0C` | `4*nargs` | Argument cells |
| following arguments | `4*nret` | Return cells |

The profile accepts 4-byte-aligned pointers, `nret >= 1`, `nargs <= 16`, `nret <= 16`, and `nargs+nret <= 16`. Length and address calculations are checked before access. The complete header, request, and response mapping are validated before a service can run. The complete request is staged before dispatch.

The response cells are staged and committed with `Memory::WriteGuestRealTransaction`, which validates all writes before touching memory. The RTAS path additionally snapshots XICS before dispatch and restores it if the response commit fails. It writes only cells defined by the service result; unused return cells and unrelated guest memory are not changed.

## Guest-memory and partition safety

The parameter pointer is interpreted as a guest-real address. Phase 18 uses the established PPC64 address model:

1. Active partition guest-real translation when an LPID context is active.
2. Configured flat radix guest-real translation when present.
3. Identity guest-real mapping for the direct profile.

Each page segment must translate and belong to the active guest. Access rejects non-RAM memory, unbacked or out-of-range regions, MMIO overlaps, protected hypervisor-handler backing, and backing ranges private to another LPID. Reads and response writes cover the full range; a later inaccessible page cannot cause a partial service call. `H_RTAS` does not accept an LPID, host pointer, or handler address from the guest.

The Phase 18 suite exercises both identity and partition translations, including a valid LPID 7 call, a source owned by LPID 8, an inaccessible cross-page request, an MMIO range, a protected hypervisor range, and response memory beyond RAM.

## Service registry and tokens

`PPC64RTASServiceRegistry` is owned by one configured `VirtualMachine` and attached to its PPC64 ISA plugin. Each descriptor carries a name, token, exact argument/return counts, capability mask, validation policy, guest-visible flag, and handler. Duplicate tokens and invalid descriptors are rejected.

The XICS profile advertises these QEMU pSeries-compatible profile tokens:

| Service | Token | Arguments | Returns |
|---|---:|---:|---:|
| `ibm,set-xive` | `0x200A` | 3 | 1 |
| `ibm,get-xive` | `0x200B` | 1 | 3 |
| `ibm,int-off` | `0x200C` | 1 | 1 |
| `ibm,int-on` | `0x200D` | 1 | 1 |

These values belong to the selected QEMU-compatible profile; they are not claimed as universal hardware token values. The guest discovers them in the FDT. The handler validates XICS server 0 and the active LPID before changing source state.

## XICS behavior

All services operate on the existing ICS source state and ICP arbitration:

- **`ibm,set-xive`** accepts only an allocated source owned by the active LPID, server 0, and priority `0x00..0xFF`. It changes that source's server and priority, updates its saved priority, enables it unless priority is `0xFF`, and preserves pending events.
- **`ibm,get-xive`** returns RTAS status, configured server, and configured source priority. It reports source priority rather than the ICP's CPPR.
- **`ibm,int-off`** saves the existing priority, masks delivery by setting the current priority to `0xFF`, and retains edge and level events.
- **`ibm,int-on`** restores the saved priority, recomputes enabled state, and reevaluates the source. It does not invent a default priority.

Invalid source, ownership, server, priority, or ABI values return RTAS `-3` and do not change source state. Guests continue to acknowledge interrupts through `H_XIRR` and complete them through `H_EOI`; existing CPPR arbitration, XIRR packing, edge latching, and level reassertion remain in XICS.

## Device-tree discovery and firmware boundary

When `rtasEnabled` is true, XICS mode is required and the generated root `/rtas` node contains:

- `compatible = "guidexos,hosted-direct-hcall-rtas-v1"`
- `rtas-version = <1>`
- The four supported service-name token properties listed above.

The FDT does **not** contain `linux,rtas-base` or `linux,rtas-entry`. It never advertises an executable address for a stub that is not present. The Phase 18 test uses an independent FDT token/string-table reader and a guest `lwz` to load the advertised `ibm,set-xive` token from guest FDT memory before constructing its call.

With RTAS disabled, the registry is absent and the FDT omits the `/rtas` service properties. The VM capability report reflects this state.

## VM lifecycle and checkpoint format

The registry is created with the platform, attached to the guest plugin, retained across pause/resume and reset, and destroyed with the VM. The registry has no mutable guest service state. Reset resets XICS configuration and reinstates configured terminal sources while preserving the selected RTAS availability. RTAS dispatch is synchronous and never waits for terminal input/output.

PPC64 checkpoint format advances from v6 to **v7** because RTAS introduces independently saved source configuration and an enabled profile bit. The v7 payload records `rtasEnabled`, each XICS source's `savedPriority` and `savedEnabled` state, including masked pending sources. It does not serialize function pointers. Restore constructs a fresh registry from the saved profile and rejects a destination with an incompatible RTAS configuration. Readers for PPC64 v1–v6 remain in the decoder; pre-v7 sources initialize `savedPriority` from their current priority and `savedEnabled` from their current enabled state. The IA-64 checkpoint format is unchanged.

Phase 18 validates v7 file save/restore and in-memory continuation both while an edge is pending under `int-off` and after guest `H_XIRR`/`H_EOI`. Historical PPC64 v1–v5 fixtures pass in the Phase 14 regression suite. The repository has no standalone v6 fixture; v6 compatibility is retained by the versioned reader path and remains a targeted regression recommendation.

## Acceptance programs A–M

| Program | Phase 18 coverage |
|---|---|
| A — H_RTAS entry | Guest constructs a block and calls `H_RTAS` via `sc 1`; outer and inner statuses checked. |
| B — token discovery | Independent FDT parsing checks all advertised tokens; guest loads the set-xive token from FDT memory. |
| C — get-xive | Server, priority, RTAS status, and BE cells checked. |
| D — set-xive | Guest changes priority and reads it back. |
| E — disable/enable | Edge pending before/during masking and level reassertion after EOI are exercised. |
| F — XICS handler | Guest enables delivery, takes the real XICS vector, executes `H_XIRR`, `H_EOI`, and `rfid`. |
| G — terminal event | RTAS configures the `hvterm1` source; injected input is read and echoed by guest code. |
| H — malformed blocks | Misalignment, counts, overflow, inaccessible pages, out-of-RAM response, MMIO, protected backing, and invalid source/return values. |
| I — unsupported services | Unknown unadvertised token returns outer `H_PARAMETER` and RTAS `-3` without XICS mutation. |
| J — two-VM isolation | Independent registries, equal source IDs, differing priorities, memory responses, and terminal input/output. |
| K — checkpoints | Pending masked source and post-EOI state restore; v7 disk round-trip checked. |
| L — console/timer | RTAS set-xive runs with PAPR terminal output and DEC activity. |
| M — end-to-end guest | Guest reads a token from FDT, configures the terminal XICS source, processes terminal input, acknowledges it, and emits `DONE`. |

All Phase 18 guest programs run through `VirtualMachine::step()` and production PPC64 instruction decoding. The service handlers are not called directly to satisfy these programs.

## Phase 18 checks and regressions

The Phase 18 executable is `tests/test_ppc64_phase18.cpp`, registered as `PPC64Phase18Tests`. Its independent instruction encoders and FDT reader report **310 checks, 0 failures**.

The PPC64 Phase 1–17 suites retain the repository's documented total of **8,333 checks**; all 17 phase executables passed in the final CTest run. Final CTest result: **26 passed and 6 Not Run** across 32 tests, with no executed test failing. The six unavailable executables are Syscall, Debugger, Profiler, Fuzzer, VMIsolation, and ELFValidation; preflight CTest had 25 passed and the same six Not Run before Phase 18 was registered.

The configured all-target build compiled the PPC64 Phase 1–18 suites but stopped while linking unrelated MinGW targets (`test_watchpoint`, `test_elf_validation`, `test_cpu_execution`, `test_mov_imm`, `debug_harness`, and `test_bootstrap`) because their REST/Web UI dependencies have unresolved WinSock `__imp_*` symbols. The Phase 18 target built and linked independently. IA-64 SAL passed in CTest; standalone IA-64 ACPI, interrupt-controller, and processor interrupt block executables also passed.

The repository separately documents historical IA-64 NaT serialization, MMU Hook-based Monitoring, and typed-memory/MMIO failures. They are outside these PPC64 phase suites. This run reproduced the documented MinGW WinSock link limitation; the six CTest executables listed above remain unavailable.

## Compatibility matrix

| Capability | Classification | Notes |
|---|---|---|
| Four XICS RTAS services | Implemented and guest-qualified | Guest machine code, real XICS state, ownership checks, and interrupt return loop pass. |
| FDT token discovery | Implemented and guest-qualified | Vendor-compatible hosted profile; no false firmware entry. |
| Direct `H_RTAS` access | Implemented for hosted direct-hypercall use | Guest begins from direct-FDT entry and calls the hypervisor. |
| RTAS registry lifecycle/checkpoint | Implemented and guest-qualified | Per-VM stateless registry; v7 captures profile and saved priority. |
| Firmware-assisted RTAS | Requires future firmware | Needs an executable guest-side forwarding trampoline before any firmware address can be advertised. |
| Linux/pSeries boot readiness | Not implemented | No SLOF/Open Firmware, Linux boot qualification, or QEMU run is claimed. |
| Full RTAS catalog, PCI, time-of-day, NVRAM, event scan, LPAR services | Not implemented | Intentionally outside this bounded profile. |
| Native XIVE, SMP, PCI, storage/network VIO | Requires future virtual-device/platform work | Existing XICS and `hvterm1` are the supported devices in this phase. |

## Recommended PPC64-19 scope

Build and qualify a guest-side RTAS forwarding trampoline with a QEMU pSeries reference run. Add a checked-in v6 checkpoint fixture so the previous checkpoint reader has a direct binary regression. Keep Linux boot claims gated on firmware and device support.
