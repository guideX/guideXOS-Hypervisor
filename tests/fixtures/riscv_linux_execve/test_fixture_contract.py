#!/usr/bin/env python3
"""Fast negative checks for the RISCV-28 ELF and newc fixture contract."""

import importlib.util
import pathlib
import struct
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[4]
BUILDER_PATH = pathlib.Path(__file__).with_name("build_execve_initramfs.py")
SPEC = importlib.util.spec_from_file_location("riscv28_fixture_builder", BUILDER_PATH)
BUILDER = importlib.util.module_from_spec(SPEC)
assert SPEC is not None and SPEC.loader is not None
SPEC.loader.exec_module(BUILDER)


class FixtureContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if len(sys.argv) != 3:
            raise RuntimeError("expected child.elf and initramfs.cpio paths")
        cls.child_path = pathlib.Path(sys.argv[1])
        cls.archive_path = pathlib.Path(sys.argv[2])
        cls.child = cls.child_path.read_bytes()
        cls.archive = cls.archive_path.read_bytes()

    def rejects_elf(self, data, expected_entry=0x20000):
        with tempfile.TemporaryDirectory(prefix="riscv28-elf-negative-") as directory:
            path = pathlib.Path(directory) / "candidate.elf"
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                BUILDER.parse_elf(path, expected_entry)

    def child_archive_name_offset(self):
        needle = b"bin/child\0"
        start = 0
        while True:
            offset = self.archive.find(needle, start)
            if offset < 0:
                self.fail("child archive path was not found")
            header = offset - 110
            if header >= 0 and self.archive[header:header + 6] == BUILDER.MAGIC:
                return offset
            start = offset + 1

    def test_invalid_elf_header(self):
        self.rejects_elf(b"BAD!" + self.child[4:])

    def test_wrong_machine_and_unsupported_format(self):
        wrong_machine = bytearray(self.child)
        struct.pack_into("<H", wrong_machine, 18, 0x3E)
        self.rejects_elf(wrong_machine)
        dynamic_executable = bytearray(self.child)
        struct.pack_into("<H", dynamic_executable, 16, 3)
        self.rejects_elf(dynamic_executable)

    def test_invalid_entrypoint(self):
        invalid = bytearray(self.child)
        struct.pack_into("<Q", invalid, 24, 0x30000)
        self.rejects_elf(invalid)

    def test_truncated_load_segment(self):
        invalid = bytearray(self.child)
        phoff = struct.unpack_from("<Q", invalid, 32)[0]
        struct.pack_into("<Q", invalid, phoff + 32, len(invalid) + 1)
        self.rejects_elf(invalid)

    def test_missing_child_path(self):
        invalid = bytearray(self.archive)
        child_name = self.child_archive_name_offset()
        invalid[child_name:child_name + len(b"bin/child\0")] = b"bin/chilD\0"
        with self.assertRaises(ValueError):
            BUILDER.parse_archive(bytes(invalid))

    def test_incorrect_child_executable_mode(self):
        invalid = bytearray(self.archive)
        child_name = self.child_archive_name_offset()
        header = child_name - 110
        invalid[header + 14:header + 22] = b"000081a4"  # 0100644
        with self.assertRaises(ValueError):
            BUILDER.parse_archive(bytes(invalid))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
