"""Synthetic ELF fixtures for the PPC64 Linux artifact verifier.

These tiny generated fixtures exercise rejection paths only. They are not Linux
kernels and are never used as the authentic artifact candidate.
"""

import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "tools" / "ppc64_linux_artifact_verify.py"


def make_elf(machine=21, endian=2, segments=None):
    if segments is None:
        segments = [(1, 5, 0x1000, 0x1000, 0x1000, 4, 8, 0x1000)]
    ident = b"\x7fELF" + bytes((2, endian, 1, 0, 0)) + bytes(7)
    header = struct.pack(
        ">16sHHIQQQIHHHHHH", ident, 2, machine, 1,
        0x1000, 64, 0, 2, 64, 56, len(segments), 0, 0, 0,
    )
    program_headers = b"".join(struct.pack(">IIQQQQQQ", *segment)
                                for segment in segments)
    image = bytearray(header + program_headers)
    image.extend(bytes(0x1000 - len(image)))
    for index, segment in enumerate(segments):
        offset, filesz = segment[2], segment[5]
        if offset + filesz > len(image):
            image.extend(bytes(offset + filesz - len(image)))
        image[offset:offset + filesz] = bytes((0x60 + index,)) * filesz
    return bytes(image)


def write_fixture(root, data):
    kernel = root / "kernel.elf"
    config = root / "config"
    kernel.write_bytes(data)
    config.write_text("CONFIG_PPC64=y\n", encoding="ascii")
    pheaders = []
    loads = []
    for segment in [(1, 5, 0x1000, 0x1000, 0x1000, 4, 8, 0x1000)]:
        ptype, flags, offset, vaddr, paddr, filesz, memsz, align = segment
        item = {
            "type": "PT_LOAD", "type_value": ptype,
            "flags": flags, "offset": hex(offset), "vaddr": hex(vaddr),
            "paddr": hex(paddr), "filesz": hex(filesz),
            "memsz": hex(memsz), "align": hex(align),
        }
        pheaders.append(item)
        loads.append(item)
    manifest = {
        "schema_version": 1,
        "artifact_id": "synthetic-test-fixture-only",
        "artifact_root_env": "UNUSED_FIXTURE_ROOT",
        "image": {
            "path": "kernel.elf", "size_bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        },
        "configuration": {
            "path": "config",
            "sha256": hashlib.sha256(config.read_bytes()).hexdigest(),
        },
        "elf": {
            "class": "ELF64", "endianness": "big",
            "machine": "EM_PPC64", "type": "ET_EXEC",
            "flags": "0x2", "entry": "0x1000",
            "program_headers": pheaders,
            "load_segments": loads,
            "bss_ranges": [{
                "paddr_start": "0x1004", "paddr_end": "0x1008",
                "vaddr_start": "0x1004", "vaddr_end": "0x1008",
            }],
            "required_ram_extent": "0x1008",
        },
    }
    manifest_path = root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return kernel, manifest_path


class ArtifactVerifierTests(unittest.TestCase):
    def run_tool(self, kernel, manifest, root):
        return subprocess.run(
            [sys.executable, str(TOOL), "--kernel", str(kernel),
             "--manifest", str(manifest), "--artifact-root", str(root)],
            text=True, capture_output=True, check=False,
        )

    def test_valid_fixture_is_accepted(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            kernel, manifest = write_fixture(root, make_elf())
            result = self.run_tool(kernel, manifest, root)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("PASS ppc64-linux-artifact-verify", result.stdout)
            self.assertIn("PT_LOAD[0]", result.stdout)

    def test_altered_image_fails_digest_check(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            kernel, manifest = write_fixture(root, make_elf())
            image = bytearray(kernel.read_bytes())
            image[-1] ^= 1
            kernel.write_bytes(image)
            result = self.run_tool(kernel, manifest, root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("SHA-256", result.stderr)

    def test_wrong_architecture_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            image = bytearray(make_elf(machine=20))
            kernel, manifest = write_fixture(root, bytes(image))
            result = self.run_tool(kernel, manifest, root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("EM_PPC64", result.stderr)

    def test_wrong_endianness_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            kernel, manifest = write_fixture(root, make_elf(endian=1))
            result = self.run_tool(kernel, manifest, root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("big-endian", result.stderr)

    def test_truncated_elf_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            truncated = make_elf()[:32]
            kernel, manifest = write_fixture(root, truncated)
            result = self.run_tool(kernel, manifest, root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("truncated", result.stderr)

    def test_overlapping_load_segments_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            segments = [
                (1, 5, 0x1000, 0x1000, 0x1000, 4, 8, 1),
                (1, 5, 0x1004, 0x2000, 0x1004, 4, 8, 1),
            ]
            kernel, manifest = write_fixture(root, make_elf(segments=segments))
            result = self.run_tool(kernel, manifest, root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("overlapping PT_LOAD", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
