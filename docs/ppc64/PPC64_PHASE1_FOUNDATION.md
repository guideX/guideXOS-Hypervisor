# PPC64 Phase 1 — ISA Foundation

Branch: `PPC64`. Phase marker: `.phase-ppc64` (the shared IA-64 `.phase` file
is untouched and still governs the IA-64 track).

Target for this phase: PowerPC 64-bit, big-endian, interpreter, IBM pSeries /
sPAPR machine model, single CPU for validation. No guest firmware, ISO, or
Linux image is required; all validation runs against hand-encoded instruction
bytes in guest memory.

## 1. Implemented ISA features

- `PPC64ISAState` (`include/PPC64ISAPlugin.h`): 32x 64-bit GPRs (`r0`–`r31`),
  NIP, LR, CTR, CR, XER, MSR. Reset zeroes everything (NIP = 0). Deep clone,
  introspection (`toString`/`dumpState`), and deterministic serialization.
- `PPC64ISAPlugin`: full `ia64::IISA` implementation —
  decode/execute/step, `getPC`/`setPC` over NIP, `setState` with
  `dynamic_cast` rejection of foreign state, `serialize_state` /
  `deserialize_state` with exact-size + magic + version validation,
  `disassemble`, capability queries (`getWordSize()` = 64,
  `isLittleEndian()` = `false`, features `interpreter`, `big_endian`,
  `fixed_width_32`, `pseries`, `spapr`).
- Decoder (`src/isa/PPC64ISAPlugin.cpp`): aligned 32-bit big-endian fetch
  through `IMemory::Read`, opcode/register/immediate extraction, signed and
  unsigned immediates with correct sign extension, I-form offset handling,
  misaligned-fetch and fetch-fault detection. Unsupported opcodes decode as
  invalid and execute as `EXCEPTION` — never silent NOPs. Stale decodes
  (PC moved between `decode` and `execute`) are rejected as `EXCEPTION`.
- Registration: static auto-registrar publishes `"PPC64"` in
  `ISAPluginRegistry`. The factory ignores its `void*` argument, so no
  IA-64 factory data (`IA64FactoryData`, decoder, syscall dispatcher,
  profiler) is ever required on this path.

## 2. Exact instruction coverage (Phase 1 subset — nothing more is claimed)

| Mnemonic | Opcode | Semantics (64-bit) |
|---|---|---|
| `addi RT,RA,SIMM` | 14 | `RT := (RA==0 ? 0 : GPR[RA]) + sext16(SIMM)` |
| `addis RT,RA,SIMM` | 15 | `RT := (RA==0 ? 0 : GPR[RA]) + sext32(SIMM \|\| 0x0000)` |
| `ori RA,RS,UIMM` | 24 | `RA := GPR[RS] \| zeroext(UIMM)` |
| `oris RA,RS,UIMM` | 25 | `RA := GPR[RS] \| (UIMM << 16)` |
| `b / bl / ba / bla` | 18 | `target := AA ? sext26(LI\|\|00) : CIA + sext26(LI\|\|00)`; `LK ? LR := CIA+4` |
| `nop` | — | conventional encoding `ori r0,r0,0` = `0x60000000` |

Notes:

- `RA == 0` in `addi`/`addis` selects the constant zero. GPR0 itself is an
  ordinary writable register; the zero-base rule never reads or alters GPR0.
- Sequential instructions advance NIP by 4. Branches set NIP to the computed
  target (wrap-around arithmetic, as on hardware).
- CR/XER/MSR/CTR storage exists for checkpoint stability; Phase 1 defines no
  MSR side effects and no condition-code or XER updates.

## 3. CPU state and serialization format

304-byte payload, explicit little-endian field encoding (host-independent):

```text
offset  size  field
0       4     magic  'P','P','C','1'  (0x31435050)
4       4     version = 1
8       256   GPR0..GPR31 (32 x u64)
264     8     NIP (u64)
272     8     LR (u64)
280     8     CTR (u64)
288     4     CR (u32)
292     4     XER (u32)
296     8     MSR (u64)
```

`deserialize_state` rejects wrong length, bad magic, and unknown versions and
leaves current state untouched. `ISAState::deserialize` throws
`std::invalid_argument` on null buffer / bad magic / bad version.

## 4. Existing Hypervisor interfaces reused

- `ia64::IISA` / `ia64::ISAState` (`include/IISA.h`) — implemented, not forked.
- `ia64::ISAPluginRegistry` (`include/ISAPluginRegistry.h`) — `"PPC64"`
  registered alongside `"IA-64"` via its own factory.
- `ia64::IMemory` / `ia64::Memory` — instruction fetch and test program
  loading go through the real memory abstraction (MMU identity-mapped).
- `tests/test_isa_plugin.cpp` memory-harness style and `tests/test_main.cpp`
  counter style informed `tests/test_ppc64_phase1.cpp`; `ExampleISAPlugin`
  served as the lifecycle reference.
- CMake (`CMakeLists.txt`): `src/isa/PPC64ISAPlugin.cpp` added to
  `ISA_SOURCES`, header to `HEADERS`, new `test_ppc64_phase1` target plus
  `PPC64Phase1Tests` ctest entry. The Visual Studio `.vcxproj` files were
  intentionally left untouched (they do not even list the existing ISA plugin
  sources; CMake is the build workflow exercised here).

## 5. IA-64 coupling discovered (full VM integration deferred)

- `VirtualMachine` owns a concrete IA-64 `InstructionDecoder`
  (`include/VirtualMachine.h`) and per-CPU `CPU` (IA-64) objects.
- `VMCheckpoint` (`src/vm/VMCheckpoint.cpp`) `dynamic_cast`s to
  `IA64ISAPlugin` and enforces bundle-boundary checkpoint rules.
- `SimpleCPUScheduler` accounts in IA-64 bundles/slots; `KernelPanic`,
  `BootTraceSystem`, and `CPUContext` snapshots carry bundle/slot state.
- Consequence: hosting PPC64 inside `VirtualMachine` needs a separate
  architectural refactor (ISA-owned fetch/step replacing decoder+bundle
  assumptions). Phase 1 therefore stops at the verified standalone plugin
  boundary. IA-64 behavior is fully preserved — no IA-64 source was modified.

## 6. Tests executed and results (MinGW-GCC 15.2 + CMake + Ninja, Debug)

New: `tests/test_ppc64_phase1.cpp` — 16 groups, **148 checks, 0 failures**:

- reset state; writable GPR0; big-endian fetch; fixed width; positive and
  negative immediates; `addi` zero-base with GPR0 = 999 still yielding
  `r3 = 42`; register arithmetic (`addi r4,r3,8`); logical immediates
  (`ori`/`oris`); 4-instruction program (`addi/addi/ori/nop` →
  `r3=42, r4=50, r5=0xFF, PC=0x10`); forward/backward/absolute branches;
  branch sign extension (`b -12`); `bl`/`bla` link updates; invalid opcodes
  (`0xFFFFFFFF`, X-form `0x7C000000`) → `EXCEPTION` with PC untouched;
  stale-decode rejection; unaligned fetch; out-of-bounds fetch; clone and
  serialization round-trip (304 bytes, magic/version/length rejection,
  foreign-state refusal); two registry-created instances with no leakage;
  registry creation with `nullptr`, `IA64FactoryData*`, and arbitrary data;
  disassembly strings including `nop` and `bl 0x90`.
- Authentic vector: bytes `38 60 00 2A` decode to `addi r3,r0,42` and execute
  to `GPR3 = 42`.

Regressions (same toolchain):

- PASS: `BasicTests` (ia64emu_tests, 10/10), `NewInstructionTests`,
  `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`,
  `RiscVDecoderSmokeTest`, `PPC64Phase1Tests`; plus standalone
  `test_ia64_interrupt_controller` and
  `test_ia64_processor_interrupt_block` ("passed").
- Pre-existing failures, unchanged by this phase (no IA-64 source modified;
  `git diff --stat` shows only `CMakeLists.txt` + new files):
  - `test_isa_plugin` aborts at `tests/test_isa_plugin.cpp:1551`:
    `IA64ISAState::serialize` (`src/isa/IA64ISAPlugin.cpp:1980`) persists GR/PR/BR/AR/IP/CFM/PSR
    but no NaT bits, so the NaT round-trip assert cannot pass. The header
    itself notes the legacy IISA payload "predates NaT … persistence".
  - The following ctest-listed and unlisted targets do not link under MinGW
    because their CMake stanzas omit `ws2_32` (MSVC gets it via
    `#pragma comment(lib, "Ws2_32.lib")`, which MinGW ignores):
    `test_cpu_execution`, `test_mov_imm`, `test_syscall_dispatcher`,
    `test_debugger`, `test_profiler`, `test_fuzzer`, `test_vm_isolation`,
    `test_vmmanager`, `test_bootstrap`, `test_io_devices`,
    `test_riscv_minimal_execution`, `test_elf_validation`, `test_watchpoint`.
    The new `test_ppc64_phase1` stanza follows the `test_isa_plugin` pattern
    and links correctly.

## 7. Explicit unsupported features

Integer/load-store/branch-conditional/floating-point/VMX-VSX/privileged
instructions; condition-code and XER updates; effective-address translation
(MMU); exceptions other than fetch/decode faults surfaced as `EXCEPTION`;
interrupts; timers; multicore; PPC64LE execution; PowerNV/OPAL; Open Firmware;
sPAPR hypercalls; device/MMIO models; full `VirtualMachine` hosting.

## 8. Remaining work toward pSeries booting (provisional roadmap)

- PPC64-2: broader integer ISA and condition codes — fixed-point arithmetic/
  logical/compare family, `cr0`/`xer` updates, conditional branches (`bc`),
  `ctr`-based branches, `mcrf`/`creg` basics, extended disassembly.
- PPC64-3: memory access and architectural exceptions — loads/stores with
  alignment and permission faults, DSI/ISI-style exception model, `sc`
  trapping surface, richer `EXCEPTION`/`SYSCALL` reporting.
- PPC64-4: privileged CPU state, MMU, and address translation — MSR
  semantics, privilege levels, SLB/hash-page-table translation behind the
  existing `IMemory`/MMU seams without disturbing IA-64 mappings.
- PPC64-5: pSeries platform, firmware interface, and hypercalls — virtual
  device tree, client-interface entry, `hcall` dispatch skeleton.
- PPC64-6+: real firmware and Linux boot investigations — SLOF/petitboot or
  sPAPR guest entry experiments, then a Linux PPC64 boot attempt.

Phase boundaries stay dependency-driven: no premature firmware work before
the integer ISA, memory model, and MMU foundations land.
