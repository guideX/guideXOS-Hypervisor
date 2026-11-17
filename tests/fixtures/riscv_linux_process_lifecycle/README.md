# RISCV-27 Linux process lifecycle fixture

This fixture is a freestanding static RV64 `/init` ELF. Linux starts it as PID
1 in U-mode. It opens `/dev/ttyS0`, writes a parent marker, initializes a
writable global, and calls the pinned Linux v6.12 RISC-V `clone` syscall with
`SIGCHLD` and all optional pointers zero. The parent and child resume from the
same ECALL with positive and zero return values respectively.

The child verifies its PID and parent PID, changes and reads back its private
copy of the global, writes a child marker, yields eight times, then calls
`exit(37)`. The parent checks its original global value, calls `wait4`, checks
the copied wait status `37 << 8`, writes a final success marker, and remains
alive in a `getpid`/`sched_yield` loop. It uses no libc, dynamic loader, helper
program, host-created task, or host-side syscall service.

Build it with the same pinned cross-toolchain, linker flags, deterministic
newc builder, and platform FDT basis used by RISCV-26:

```bash
tests/fixtures/riscv_linux_process_lifecycle/build.sh \
  /mnt/e/CodexArtifacts/guideXOS_Hypervisor/phase-riscv27-artifacts/inputs \
  tests/fixtures/riscv_linux_image/platform.dts
```

The ELF uses separate RX and RW PT_LOAD segments. The initramfs remains a
single `/init` with the original `/dev/console` and `/dev/ttyS0` nodes. The
builder records source, tool versions, commands, ELF and CPIO identities,
fixed ownership/timestamps, entry ordering, and process-proof parameters.
