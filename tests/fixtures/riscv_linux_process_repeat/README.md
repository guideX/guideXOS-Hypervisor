# RISCV-29 repeated Linux process fixture

This deterministic static userspace fixture reuses the RISCV-28 newc archive
and ELF validation implementation. PID 1 creates two sequential children. The
first executes `/bin/child`, which validates Linux's argv/environment stack
and exits 42. The second modifies a private sentinel, calls `execve` on the
absent `/bin/riscv29-missing`, verifies Linux returned `-ENOENT` in the
original image, checks its PID/parent/sentinel, then exits 43.

Build into a new external artifact directory:

```sh
tests/fixtures/riscv_linux_process_repeat/build.sh \
  /path/to/phase-riscv29-artifacts/inputs \
  tests/fixtures/riscv_linux_image/platform.dts
```

The archive includes only the root, `/bin`, `/dev`, `/dev/console`,
`/dev/ttyS0`, `/init`, and `/bin/child`. The missing exec path is absent.
