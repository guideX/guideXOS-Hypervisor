#!/usr/bin/env python3
"""Negative and reproducibility checks for the RISCV-29 process fixture."""

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True


FIXTURE = pathlib.Path(__file__).resolve().parent
BUILDER_PATH = FIXTURE / "build_repeat_initramfs.py"
SPEC = importlib.util.spec_from_file_location("riscv29_initramfs_builder", BUILDER_PATH)
BUILDER = importlib.util.module_from_spec(SPEC)
assert SPEC is not None and SPEC.loader is not None
SPEC.loader.exec_module(BUILDER)
ACCEPTED_PATH = FIXTURE.parent / "riscv_linux_execve" / "build_execve_initramfs.py"
ACCEPTED_SPEC = importlib.util.spec_from_file_location(
    "riscv29_accepted_archive_parser", ACCEPTED_PATH)
ACCEPTED = importlib.util.module_from_spec(ACCEPTED_SPEC)
assert ACCEPTED_SPEC is not None and ACCEPTED_SPEC.loader is not None
ACCEPTED_SPEC.loader.exec_module(ACCEPTED)


class ProcessRepeatFixtureContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if len(sys.argv) != 5:
            raise RuntimeError("expected init.elf, child.elf, initramfs.cpio, manifest")
        cls.init_path, cls.child_path, cls.archive_path, cls.manifest_path = map(
            pathlib.Path, sys.argv[1:])
        cls.init = cls.init_path.read_bytes()
        cls.child = cls.child_path.read_bytes()
        cls.archive = cls.archive_path.read_bytes()
        cls.manifest = json.loads(cls.manifest_path.read_text(encoding="utf-8"))

    def test_manifest_describes_second_recovery_lifecycle(self):
        proof = self.manifest["processProof"]
        self.assertEqual(self.manifest["phase"], "RISCV-29")
        self.assertEqual(proof["argv"], ["/bin/child", "execve-riscv29"])
        self.assertEqual(proof["envp"], ["GUIDEXOS_RISCV29=present"])
        self.assertEqual(proof["missingExecvePath"], "/bin/riscv29-missing")
        self.assertEqual(proof["missingExecveErrno"], -2)
        self.assertEqual(proof["secondChildExitCode"], 43)
        self.assertEqual(proof["secondWaitStatus"], 11008)

    def test_missing_executable_is_not_packaged(self):
        names = [entry["name"] for entry in ACCEPTED.parse_archive(self.archive)]
        self.assertNotIn("bin/riscv29-missing", names)
        self.assertIn("bin/child", names)

    def test_builder_reproduces_archive_and_manifest(self):
        with tempfile.TemporaryDirectory(prefix="riscv29-fixture-contract-") as tmp:
            root = pathlib.Path(tmp)
            archive = root / "initramfs.cpio"
            manifest = root / "manifest.json"
            BUILDER.build(self.init_path, self.child_path, archive, manifest)
            self.assertEqual(archive.read_bytes(), self.archive)
            generated = json.loads(manifest.read_text(encoding="utf-8"))
            self.assertEqual(generated, self.manifest)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
