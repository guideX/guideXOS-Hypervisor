# RISCV-26 minimal Linux initramfs

This fixture builds a freestanding RV64 `/init` and an uncompressed deterministic
`newc` CPIO archive. The program uses Linux RISC-V U-mode `getpid` (172),
`openat` (56), `write` (64), and `sched_yield` (124) system calls. It checks PID
1, opens the configured serial TTY at `/dev/ttyS0`, writes the fixed proof
message to the descriptor Linux returns, checks the byte count, and stays
alive in a bounded-by-the-harness yield loop.

Build in WSL with the pinned Linux source toolchain packages installed:

```bash
tests/fixtures/riscv_linux_initramfs/build.sh \
  /mnt/e/CodexArtifacts/guideXOS_Hypervisor/phase-riscv26-artifacts/inputs \
  tests/fixtures/riscv_linux_image/platform.dts
```

`build.sh` uses `riscv64-linux-gnu-gcc`, `readelf`, Python 3, and `dtc`. It
builds the static ELF without libc, validates ELF and CPIO independently, and
generates a RISCV-26 FDT with exact 64-bit `/chosen/linux,initrd-start` and
exclusive `linux,initrd-end` properties. The RISCV-26 copy of the SoC node
advertises the standard `simple-bus` compatibility so Linux populates UART
platform devices from the bus; the RISCV-18 base DTS remains unchanged. Output
binaries remain outside the repository. The archive contains only `.`, `dev`,
`dev/console` (character device 5:1), `dev/ttyS0` (character device 4:64),
`init`, and the required `TRAILER!!!` record. `/dev/ttyS0` targets the standard
first 8250 TTY on the UART selected by the Linux boot arguments. All ownership
is root, timestamps are zero, and inode numbering and entry order are fixed.

The syscall numbers are from the pinned v6.12 source's
`include/uapi/asm-generic/unistd.h`; the Linux RISC-V UAPI includes these
generic numbers through `arch/riscv/include/uapi/asm/unistd.h`.
