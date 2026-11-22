#!/usr/bin/env python3
"""Reproducibility and static-ELF contract for RISCV-30's console fixture."""

import importlib.util
import json
import pathlib
import struct
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
FIXTURE = pathlib.Path(__file__).resolve().parent
BUILDER_PATH = FIXTURE / "build_serial_initramfs.py"
SPEC = importlib.util.spec_from_file_location("riscv30_serial_builder", BUILDER_PATH)
BUILDER = importlib.util.module_from_spec(SPEC)
assert SPEC is not None and SPEC.loader is not None
SPEC.loader.exec_module(BUILDER)
ACCEPTED_PATH = FIXTURE.parent / "riscv_linux_initramfs" / "build_initramfs.py"
ACCEPTED_SPEC = importlib.util.spec_from_file_location(
    "riscv30_accepted_newc_builder", ACCEPTED_PATH)
ACCEPTED = importlib.util.module_from_spec(ACCEPTED_SPEC)
assert ACCEPTED_SPEC is not None and ACCEPTED_SPEC.loader is not None
ACCEPTED_SPEC.loader.exec_module(ACCEPTED)


class SerialFixtureContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if len(sys.argv) != 4:
            raise RuntimeError("expected init.elf, initramfs.cpio, manifest")
        cls.init_path, cls.archive_path, cls.manifest_path = map(pathlib.Path, sys.argv[1:])
        cls.init = cls.init_path.read_bytes()
        cls.archive = cls.archive_path.read_bytes()
        cls.manifest = json.loads(cls.manifest_path.read_text(encoding="utf-8"))

    def test_manifest_declares_bounded_serial_protocol(self):
        self.assertEqual(self.manifest["phase"], "RISCV-30")
        protocol = self.manifest["serialProtocol"]
        self.assertEqual(protocol["device"], "/dev/ttyS0")
        self.assertEqual(protocol["openFlags"], "O_RDWR")
        self.assertEqual(protocol["bufferBytes"], 64)
        self.assertTrue(protocol["requiresNewline"])
        self.assertTrue(protocol["unknownCommandContinues"])
        self.assertTrue(protocol["quitLeavesPid1Alive"])
        self.assertEqual(protocol["commands"],
                         ["ping", "status", "echo hello", "unrecognized", "quit"])

    def test_archive_contains_only_minimal_console_nodes_and_init(self):
        entries = ACCEPTED.validate_archive(self.archive)
        self.assertEqual([item["name"] for item in entries],
                         [".", "dev", "dev/console", "dev/ttyS0", "init", "TRAILER!!!"])
        self.assertEqual(entries[3]["rdevmajor"], 4)
        self.assertEqual(entries[3]["rdevminor"], 64)
        self.assertEqual(entries[4]["filesize"], len(self.init))

    def test_init_is_static_rv64_and_exports_console_proof_labels(self):
        elf = ACCEPTED.parse_elf(self.init_path)
        self.assertEqual(elf["entry"], 0x10000)
        self.assertIsNone(elf["interpreter"])
        self.assertEqual(self.init[4:6], b"\x02\x01")
        (etype, machine) = struct.unpack_from("<HH", self.init, 16)
        self.assertEqual((etype, machine), (2, 243))
        symbols = ACCEPTED.parse_elf(self.init_path)["loadSegments"]
        self.assertTrue(symbols)
        for label in (b"riscv30_openat_ecall", b"riscv30_read_ecall",
                      b"riscv30_command_ping", b"riscv30_command_status",
                      b"riscv30_command_echo", b"riscv30_command_unknown",
                      b"riscv30_command_quit", b"riscv30_quit_yield_loop"):
            self.assertIn(label, self.init)

    def test_builder_reproduces_archive_and_manifest(self):
        with tempfile.TemporaryDirectory(prefix="riscv30-serial-fixture-") as tmp:
            root = pathlib.Path(tmp)
            archive = root / "initramfs.cpio"
            manifest = root / "manifest.json"
            BUILDER.build(self.init_path, archive, manifest)
            self.assertEqual(archive.read_bytes(), self.archive)
            self.assertEqual(json.loads(manifest.read_text(encoding="utf-8")),
                             self.manifest)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
