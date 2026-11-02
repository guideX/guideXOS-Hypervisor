#!/usr/bin/env python3
"""Verify a PPC64 big-endian Linux ELF against an explicit JSON manifest.

The verifier is offline and uses only the Python standard library. It checks
the image and configuration digests before comparing the ELF64 header and the
complete program-header table with the manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
from pathlib import Path, PurePosixPath
from typing import Any


U64_MAX = (1 << 64) - 1
MAX_PROGRAM_HEADERS = 4096
ELF_HEADER_SIZE = 64
ELF64_PROGRAM_HEADER_SIZE = 56
PT_NAMES = {
    0: "PT_NULL",
    1: "PT_LOAD",
    2: "PT_DYNAMIC",
    3: "PT_INTERP",
    4: "PT_NOTE",
    5: "PT_SHLIB",
    6: "PT_PHDR",
    7: "PT_TLS",
    0x6474E550: "PT_GNU_EH_FRAME",
    0x6474E551: "PT_GNU_STACK",
    0x6474E552: "PT_GNU_RELRO",
}


class VerificationError(Exception):
    """A bounded, user-actionable validation error."""


def _fail(message: str) -> None:
    raise VerificationError(message[:512])


def _parse_integer(value: Any, field: str) -> int:
    if isinstance(value, bool):
        _fail(f"manifest field {field} must be an integer")
    if isinstance(value, int):
        parsed = value
    elif isinstance(value, str):
        try:
            parsed = int(value, 0)
        except ValueError:
            _fail(f"manifest field {field} is not an integer")
    else:
        _fail(f"manifest field {field} must be an integer")
    if parsed < 0 or parsed > U64_MAX:
        _fail(f"manifest field {field} is outside the ELF64 range")
    return parsed


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as exc:
        _fail(f"cannot read {path.name}: {exc}")
    return digest.hexdigest()


def _relative_artifact(root: Path, value: Any, field: str) -> Path:
    if not isinstance(value, str) or not value:
        _fail(f"manifest field {field} must be a relative path")
    rel = PurePosixPath(value)
    if rel.is_absolute() or any(part in ("", ".", "..") for part in rel.parts):
        _fail(f"manifest field {field} must stay within the artifact root")
    resolved_root = root.resolve()
    resolved = resolved_root.joinpath(*rel.parts).resolve()
    if not resolved.is_relative_to(resolved_root):
        _fail(f"manifest field {field} escapes the artifact root")
    return resolved


def _hex(value: int) -> str:
    return f"0x{value:x}"


def _type_name(value: int) -> str:
    return PT_NAMES.get(value, f"PT_{value:#x}")


def _parse_elf(path: Path, file_size: int) -> dict[str, Any]:
    try:
        with path.open("rb") as stream:
            ident = stream.read(16)
            if len(ident) != 16:
                _fail("truncated ELF identification")
            if ident[:4] != b"\x7fELF":
                _fail("unsupported image format: input is not ELF")
            if ident[4] != 2:
                _fail("unsupported ELF class: expected ELF64")
            if ident[5] != 2:
                _fail("unsupported ELF endianness: expected big-endian")
            if ident[6] != 1:
                _fail("unsupported ELF identification version")

            header_rest = stream.read(ELF_HEADER_SIZE - 16)
            if len(header_rest) != ELF_HEADER_SIZE - 16:
                _fail("truncated ELF64 header")
            fields = struct.unpack(">HHIQQQIHHHHHH", header_rest)
            (elf_type, machine, version, entry, phoff, shoff, flags,
             ehsize, phentsize, phnum, shentsize, shnum,
             shstrndx) = fields
            del shoff, shentsize, shnum, shstrndx

            if version != 1 or ehsize != ELF_HEADER_SIZE:
                _fail("unsupported ELF64 header version or size")
            if elf_type != 2:
                _fail("unsupported ELF type: expected ET_EXEC")
            if machine != 21:
                _fail("unsupported ELF machine: expected EM_PPC64")
            if phnum in (0, 0xFFFF) or phnum > MAX_PROGRAM_HEADERS:
                _fail("unsupported or unbounded ELF program-header count")
            if phentsize != ELF64_PROGRAM_HEADER_SIZE:
                _fail("unsupported ELF64 program-header entry size")
            if phoff > file_size or phnum * phentsize > file_size - phoff:
                _fail("truncated ELF program-header table")

            stream.seek(phoff)
            program_headers: list[dict[str, Any]] = []
            for _ in range(phnum):
                raw = stream.read(phentsize)
                if len(raw) != phentsize:
                    _fail("truncated ELF program header")
                (ptype, pflags, poffset, pvaddr, ppaddr,
                 pfilesz, pmemsz, palign) = struct.unpack(">IIQQQQQQ", raw)
                if poffset > file_size or pfilesz > file_size - poffset:
                    _fail("ELF program-header file range is outside the image")
                if pmemsz < pfilesz:
                    _fail("ELF program-header p_memsz is smaller than p_filesz")
                if (pvaddr > U64_MAX - pmemsz or
                        ppaddr > U64_MAX - pmemsz):
                    _fail("ELF program-header memory range overflows ELF64")
                if palign not in (0, 1):
                    if palign & (palign - 1):
                        _fail("ELF program-header p_align is not a power of two")
                    if poffset % palign != pvaddr % palign:
                        _fail("ELF p_offset/p_vaddr alignment congruence failed")
                    if poffset % palign != ppaddr % palign:
                        _fail("ELF p_offset/p_paddr alignment congruence failed")
                program_headers.append({
                    "type": _type_name(ptype),
                    "type_value": ptype,
                    "flags": pflags,
                    "offset": poffset,
                    "vaddr": pvaddr,
                    "paddr": ppaddr,
                    "filesz": pfilesz,
                    "memsz": pmemsz,
                    "align": palign,
                })
    except OSError as exc:
        _fail(f"cannot inspect ELF image: {exc}")

    loads = [header for header in program_headers if header["type_value"] == 1]
    if not loads:
        _fail("ELF image has no PT_LOAD segments")

    physical_ranges: list[tuple[int, int]] = []
    virtual_ranges: list[tuple[int, int]] = []
    bss_ranges: list[dict[str, int]] = []
    ram_extent = 0
    for segment in loads:
        paddr_end = segment["paddr"] + segment["memsz"]
        vaddr_end = segment["vaddr"] + segment["memsz"]
        physical_ranges.append((segment["paddr"], paddr_end))
        virtual_ranges.append((segment["vaddr"], vaddr_end))
        ram_extent = max(ram_extent, paddr_end)
        if segment["memsz"] > segment["filesz"]:
            bss_ranges.append({
                "paddr_start": segment["paddr"] + segment["filesz"],
                "paddr_end": paddr_end,
                "vaddr_start": segment["vaddr"] + segment["filesz"],
                "vaddr_end": vaddr_end,
            })

    for ranges, label in ((physical_ranges, "physical"),
                          (virtual_ranges, "virtual")):
        ordered = sorted(ranges)
        for previous, current in zip(ordered, ordered[1:]):
            if current[0] < previous[1]:
                _fail(f"overlapping PT_LOAD {label} memory ranges")

    executable_entry = any(
        segment["flags"] & 1 and
        segment["vaddr"] <= entry < segment["vaddr"] + segment["memsz"]
        for segment in loads
    )
    if not executable_entry:
        _fail("ELF entry does not lie in an executable PT_LOAD segment")

    return {
        "class": "ELF64",
        "endianness": "big",
        "machine": "EM_PPC64",
        "type": "ET_EXEC",
        "entry": entry,
        "flags": flags,
        "program_headers": program_headers,
        "load_segments": loads,
        "bss_ranges": bss_ranges,
        "required_ram_extent": ram_extent,
    }


def _expected_program_header(item: Any, index: int) -> dict[str, Any]:
    if not isinstance(item, dict):
        _fail(f"manifest program_headers[{index}] must be an object")
    expected_type = item.get("type")
    if not isinstance(expected_type, str):
        _fail(f"manifest program_headers[{index}].type must be a string")
    return {
        "type": expected_type,
        "type_value": _parse_integer(item.get("type_value"),
                                      f"program_headers[{index}].type_value"),
        "flags": _parse_integer(item.get("flags"),
                                 f"program_headers[{index}].flags"),
        "offset": _parse_integer(item.get("offset"),
                                  f"program_headers[{index}].offset"),
        "vaddr": _parse_integer(item.get("vaddr"),
                                f"program_headers[{index}].vaddr"),
        "paddr": _parse_integer(item.get("paddr"),
                                f"program_headers[{index}].paddr"),
        "filesz": _parse_integer(item.get("filesz"),
                                 f"program_headers[{index}].filesz"),
        "memsz": _parse_integer(item.get("memsz"),
                                f"program_headers[{index}].memsz"),
        "align": _parse_integer(item.get("align"),
                                f"program_headers[{index}].align"),
    }


def _compare_manifest(elf: dict[str, Any], manifest: dict[str, Any]) -> None:
    expected = manifest.get("elf")
    if not isinstance(expected, dict):
        _fail("manifest is missing the elf object")
    for key in ("class", "endianness", "machine", "type"):
        if expected.get(key) != elf[key]:
            _fail(f"ELF {key} does not match the manifest")
    for key in ("entry", "flags"):
        if _parse_integer(expected.get(key), f"elf.{key}") != elf[key]:
            _fail(f"ELF {key} does not match the manifest")

    header_records = expected.get("program_headers")
    if not isinstance(header_records, list):
        _fail("manifest elf.program_headers must be an array")
    expected_headers = [
        _expected_program_header(record, index)
        for index, record in enumerate(header_records)
    ]
    if expected_headers != elf["program_headers"]:
        _fail("ELF program-header table does not match the manifest")

    expected_loads = [
        _expected_program_header(record, index)
        for index, record in enumerate(expected.get("load_segments", []))
    ]
    if expected_loads != elf["load_segments"]:
        _fail("ELF PT_LOAD table does not match the manifest")

    expected_bss = expected.get("bss_ranges")
    if not isinstance(expected_bss, list):
        _fail("manifest elf.bss_ranges must be an array")
    normalized_bss = []
    for index, item in enumerate(expected_bss):
        if not isinstance(item, dict):
            _fail(f"manifest bss_ranges[{index}] must be an object")
        normalized_bss.append({
            key: _parse_integer(item.get(key), f"bss_ranges[{index}].{key}")
            for key in ("paddr_start", "paddr_end", "vaddr_start", "vaddr_end")
        })
    if normalized_bss != elf["bss_ranges"]:
        _fail("ELF BSS ranges do not match the manifest")

    if (_parse_integer(expected.get("required_ram_extent"),
                       "elf.required_ram_extent") !=
            elf["required_ram_extent"]):
        _fail("ELF required RAM extent does not match the manifest")


def verify(kernel_arg: Path, manifest_path: Path,
           artifact_root_arg: Path | None) -> list[str]:
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        _fail(f"cannot read manifest: {exc}")
    if not isinstance(manifest, dict) or manifest.get("schema_version") != 1:
        _fail("unsupported artifact manifest schema")

    env_name = manifest.get("artifact_root_env")
    root = artifact_root_arg
    if root is None and isinstance(env_name, str):
        env_value = os.environ.get(env_name)
        if env_value:
            root = Path(env_value)
    if root is None:
        _fail("set --artifact-root or the manifest's artifact-root environment variable")

    image_record = manifest.get("image")
    if not isinstance(image_record, dict):
        _fail("manifest is missing the image object")
    expected_image = _relative_artifact(root, image_record.get("path"), "image.path")
    kernel = kernel_arg.resolve()
    if kernel != expected_image:
        _fail("explicit kernel path does not resolve to manifest image.path")

    if not kernel.is_file():
        _fail("explicit kernel path is not a regular file")
    actual_size = kernel.stat().st_size
    expected_size = _parse_integer(image_record.get("size_bytes"),
                                   "image.size_bytes")
    if actual_size != expected_size:
        _fail("kernel size does not match the manifest")
    expected_digest = image_record.get("sha256")
    if not isinstance(expected_digest, str) or not re.fullmatch(
            r"[0-9a-fA-F]{64}", expected_digest):
        _fail("manifest image.sha256 is not a SHA-256 digest")
    actual_digest = _sha256(kernel)
    if actual_digest.lower() != expected_digest.lower():
        _fail("kernel SHA-256 does not match the manifest")

    config_record = manifest.get("configuration")
    if not isinstance(config_record, dict):
        _fail("manifest is missing the configuration object")
    config_path = _relative_artifact(root, config_record.get("path"),
                                     "configuration.path")
    config_digest = config_record.get("sha256")
    if not isinstance(config_digest, str) or not re.fullmatch(
            r"[0-9a-fA-F]{64}", config_digest):
        _fail("manifest configuration.sha256 is not a SHA-256 digest")
    if _sha256(config_path).lower() != config_digest.lower():
        _fail("kernel configuration SHA-256 does not match the manifest")

    elf = _parse_elf(kernel, actual_size)
    _compare_manifest(elf, manifest)
    lines = [
        "PASS ppc64-linux-artifact-verify",
        f"artifact_id={manifest.get('artifact_id', 'unknown')}",
        f"kernel_sha256={actual_digest}",
        f"config_sha256={config_digest.lower()}",
        (f"elf=ELF64 big-endian EM_PPC64 ET_EXEC flags={_hex(elf['flags'])} "
         f"entry={_hex(elf['entry'])}"),
    ]
    for index, segment in enumerate(elf["load_segments"]):
        lines.append(
            f"PT_LOAD[{index}] off={_hex(segment['offset'])} "
            f"vaddr={_hex(segment['vaddr'])} paddr={_hex(segment['paddr'])} "
            f"filesz={_hex(segment['filesz'])} memsz={_hex(segment['memsz'])} "
            f"flags={_hex(segment['flags'])} align={_hex(segment['align'])}"
        )
    for index, bss in enumerate(elf["bss_ranges"]):
        lines.append(
            f"BSS[{index}] paddr=[{_hex(bss['paddr_start'])},"
            f"{_hex(bss['paddr_end'])})"
        )
    lines.append(f"required_ram_extent={_hex(elf['required_ram_extent'])}")
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path,
                        help="explicit path to the immutable kernel image")
    parser.add_argument("--manifest", required=True, type=Path,
                        help="JSON artifact manifest")
    parser.add_argument("--artifact-root", type=Path,
                        help="root for relative paths in the manifest")
    args = parser.parse_args(argv)
    try:
        for line in verify(args.kernel, args.manifest, args.artifact_root):
            print(line)
        return 0
    except VerificationError as exc:
        print(f"FAIL ppc64-linux-artifact-verify: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
