# RISCV-30 authentic Linux serial console fixture

This fixture is a static RV64 `/init` for Linux v6.12. PID 1 opens
`/dev/ttyS0` read/write, prompts for one newline-terminated command at a time,
and interprets `ping`, `status`, `echo hello`, an unknown command, and `quit`.
It has a fixed 64-byte input buffer, no dynamic linker, no shell, and no other
userspace programs. The quit acknowledgement moves PID 1 into a `sched_yield`
loop so the harness can stop with the process alive.

Build into the RISCV-30 artifact inputs directory:

```sh
tests/fixtures/riscv_linux_serial_console/build.sh \
  /path/to/phase-riscv30-artifacts/inputs \
  tests/fixtures/riscv_linux_image/platform.dts
```

The builder reuses the accepted deterministic uncompressed newc implementation
and includes only `.`, `/dev`, `/dev/console`, `/dev/ttyS0`, `/init`, and the
newc trailer. The platform FDT retains the established RAM, UART, PLIC,
ACLINT, boot arguments, and `/soc` `simple-bus` contract.
