#!/usr/bin/env python3
"""Reuse the accepted deterministic newc builder with RISCV-29 proof metadata."""

import importlib.util
import json
import pathlib
import sys

sys.dont_write_bytecode = True


def build(init: pathlib.Path, child: pathlib.Path, archive: pathlib.Path,
          manifest: pathlib.Path) -> None:
    fixture = pathlib.Path(__file__).resolve().parent
    accepted = fixture.parent / "riscv_linux_execve" / "build_execve_initramfs.py"
    spec = importlib.util.spec_from_file_location("riscv28_initramfs", accepted)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load accepted deterministic initramfs builder")
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    builder.build(init, child, archive, manifest)
    document = json.loads(manifest.read_text(encoding="utf-8"))
    document["phase"] = "RISCV-29"
    document["processProof"].update({
        "argv": ["/bin/child", "execve-riscv29"],
        "envp": ["GUIDEXOS_RISCV29=present"],
        "secondCloneFlags": 17,
        "missingExecvePath": "/bin/riscv29-missing",
        "missingExecveErrno": -2,
        "secondChildExitCode": 43,
        "secondWaitStatus": 11008,
    })
    manifest.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    if len(sys.argv) != 6 or sys.argv[1] != "build":
        raise SystemExit("usage: build_repeat_initramfs.py build INIT CHILD CPIO MANIFEST")
    build(*map(pathlib.Path, sys.argv[2:]))


if __name__ == "__main__":
    main()
