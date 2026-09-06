# IA-64 post-EFI VM checkpoints

The IA-64 checkpoint facility is an opt-in diagnostic path for eliminating the
large deterministic EFI replay cost during kernel investigations. It is
disabled by default and does not alter normal VM execution.

## Boundary

The write path runs the guest normally, including `ExitBootServices`, and
records the first clean stage-180 handoff in which the EFI boot-services call
has returned successfully and control has branched/called into a valid
non-firmware target. The checkpoint is taken immediately after that transfer
has been committed. No instruction is skipped and no guest register or memory
value is synthesized.

## Format

The file format is version 1 (`GXIA64VM`) and is little-endian with an explicit
endian marker. The header records the IA-64 architecture, machine dimensions,
RAM size, MMU/device configuration, checkpoint IP/slot, guest identity, build
identity, payload length, and SHA-256. The payload contains the complete
checkpoint state followed by exact guest RAM bytes.

The IA-64 plugin state is independently versioned (blob version 3). Version 2
is accepted for compatibility with the first checkpoint artifact; it restores
the same default CPUID profile used when that artifact was written. Version 3
also records the modeled CPUID registers explicitly. The blob includes the
architectural CPU state, runtime execution state, RSE and NaT state, EFI
post-handoff state, loaded-image and file/protocol state, memory-map
allocations, boot image bytes, and diagnostic counters needed to make the
post-handoff continuation deterministic. The FAT parser is rebuilt from the
serialized boot image rather than treated as an independent mutable source.

## State coverage

- CPU: GR/NaT, FR, predicates, branch registers, IP/slot, PSR, CFM, control and
  application registers, region registers, translation registers, loop/timer
  registers, interruption state, and RSE/backing-store state.
- Translation: the VM MMU page table is serialized exactly, including the
  modeled IA-64 region-6 direct-map alias used by this guest. Translation
  caches are not architectural state in this emulator and are rebuilt by
  normal lookup; ALAT is not modeled architecturally and is therefore not
  serialized.
- Memory: all configured guest physical RAM bytes are serialized byte-for-byte.
- Platform: console buffers, framebuffer, timer and interrupt-controller state,
  scheduler state, boot-state state, and mutable EFI/plugin protocol, file,
  allocation, memory-map, and loaded-image state are serialized.
- Events: CPU pending interrupt/call-input state and serialized timer/
  interrupt-controller state are restored. Host callback closures are rebound
  by the VM rather than persisted as pointers.

## CLI and validation

`ia64_iso_matrix` supports `--checkpoint-write <file>` and
`--checkpoint-read <file>`, with `--handoff-cycles`,
`--post-checkpoint-cycles`, and `--equivalence-log` for bounded diagnostics.
The write path also runs a same-process control/restore continuation check.

Restore rejects bad magic, unsupported format or plugin versions, wrong
endianness, build/architecture/kind mismatch, guest identity mismatch, RAM or
machine-configuration mismatch, malformed lengths/counts/enums, duplicate
translation or map entries, invalid runtime shapes, truncation, trailing
payload bytes, and SHA-256 mismatch. Parsing is bounded and no partial or
silently lossy restore is accepted.

The facility is intended to be used with strict architectural checks enabled.
It must never be used to jump directly to A5D, patch guest RAM, inject expected
register values, or skip required guest instructions.
