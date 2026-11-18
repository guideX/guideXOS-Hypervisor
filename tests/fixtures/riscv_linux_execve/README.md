# RISCV-28 Linux execve fixture

This fixture contains two independently assembled, static RV64 ET_EXEC ELFs.
Linux starts `/init` at `0x10000`; PID 1 creates a fork-style child with
`clone(SIGCHLD)`. The child runs U-mode code in the original image, verifies
its PID, parent PID, and private memory, then invokes Linux `execve(221)` with
the user-space pathname `/bin/child`, `argv = {"/bin/child",
"execve-riscv28", NULL}`, and `envp = {"GUIDEXOS_RISCV28=present", NULL}`.

Linux must load the separate executable linked at `0x20000`, construct its
initial stack, and return to U-mode at that entry. The new image checks
`argc`, both arguments, the terminating argv pointer, and its environment
entry directly from the Linux-created stack. It then calls `getpid`,
`getppid`, `openat("/dev/ttyS0")`, `write`, and `exit(42)`. PID 1 verifies
copy-on-write isolation, reaps the same child with `wait4`, reads status
10752 from its own memory, checks `-ECHILD`, emits a success marker, and stays
alive for the bounded observer.

`build.sh` uses the pinned RV64IMA toolchain settings and the deterministic
newc builder in this directory. The builder independently validates both
ELF headers and PT_LOAD ranges, no interpreter or dynamic segment, entrypoint,
page alignment, archive metadata, paths, and executable file modes. The
resulting initramfs adds only `/bin` and `/bin/child` to the accepted console
profile. No host loader, task creation, SBI provider, or userspace handoff is
used for the guest execution.
