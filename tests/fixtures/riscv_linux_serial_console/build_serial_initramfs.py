#!/usr/bin/env python3
"""Build RISCV-30's deterministic minimal serial-console initramfs."""

import importlib.util
import json
import pathlib
import sys

sys.dont_write_bytecode = True


def build(elf_path: pathlib.Path, archive_path: pathlib.Path,
          manifest_path: pathlib.Path) -> None:
    accepted = pathlib.Path(__file__).resolve().parents[1] / "riscv_linux_initramfs" / \
        "build_initramfs.py"
    spec = importlib.util.spec_from_file_location("riscv26_newc_builder", accepted)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load accepted deterministic newc builder")
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    builder.build(elf_path, archive_path, manifest_path)

    document = json.loads(manifest_path.read_text(encoding="utf-8"))
    document["phase"] = "RISCV-30"
    document["serialProtocol"] = {
        "device": "/dev/ttyS0",
        "openFlags": "O_RDWR",
        "bufferBytes": 64,
        "lineTermination": "LF",
        "commands": ["ping", "status", "echo hello", "unrecognized", "quit"],
        "responses": ["pong\n", "status:pid=1\n", "hello\n",
                      "unknown command\n", "quit acknowledged\n"],
        "requiresNewline": True,
        "unknownCommandContinues": True,
        "quitLeavesPid1Alive": True,
    }
    manifest_path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    if len(sys.argv) != 5 or sys.argv[1] != "build":
        raise SystemExit("usage: build_serial_initramfs.py build ELF CPIO MANIFEST")
    build(*map(pathlib.Path, sys.argv[2:]))


if __name__ == "__main__":
    main()
