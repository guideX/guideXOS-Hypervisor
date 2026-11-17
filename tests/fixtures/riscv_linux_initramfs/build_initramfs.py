#!/usr/bin/env python3
"""Build and independently validate the deterministic RISCV-26 newc archive."""

import hashlib
import json
import pathlib
import struct
import sys


MAGIC = b"070701"
TRAILER = "TRAILER!!!"
FIXED_MTIME = 0


def align4(value: int) -> int:
    return (value + 3) & ~3


def parse_elf(path: pathlib.Path) -> dict:
    data = path.read_bytes()
    if len(data) < 64:
        raise ValueError("ELF is shorter than its ELF64 header")
    ident = data[:16]
    fields = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    (etype, machine, version, entry, phoff, shoff, flags, ehsize,
     phentsize, phnum, shentsize, shnum, shstrndx) = fields
    del version, shoff, shentsize, shnum, shstrndx
    if ident[:4] != b"\x7fELF" or ident[4:6] != b"\x02\x01":
        raise ValueError("/init must be ELF64 little-endian")
    if etype != 2 or machine != 243 or ehsize != 64:
        raise ValueError("/init must be a static RISC-V ET_EXEC ELF64")
    if ident[7] != 0 or ident[8] != 0:
        raise ValueError("unexpected ELF ABI or version identity")
    if phentsize != 56 or phoff + phentsize * phnum > len(data):
        raise ValueError("invalid or truncated ELF program-header table")
    loads = []
    has_interp = False
    for index in range(phnum):
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = \
            struct.unpack_from("<IIQQQQQQ", data, phoff + index * phentsize)
        del p_paddr
        if p_type == 3:
            has_interp = True
        if p_type == 1:
            if p_offset + p_filesz > len(data) or p_filesz > p_memsz:
                raise ValueError("PT_LOAD extent exceeds the ELF file or memory size")
            loads.append({"vaddr": p_vaddr, "filesz": p_filesz,
                          "memsz": p_memsz, "flags": p_flags,
                          "offset": p_offset, "align": p_align})
    if has_interp or not loads:
        raise ValueError("/init must have PT_LOAD and no PT_INTERP")
    if not any((segment["flags"] & 1) and
               segment["vaddr"] <= entry < segment["vaddr"] + segment["memsz"]
               for segment in loads):
        raise ValueError("ELF entrypoint is not inside an executable PT_LOAD")
    if flags & 1:
        raise ValueError("ELF advertises the RISC-V compressed-instruction ABI")
    return {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data),
            "entry": entry, "type": etype, "machine": machine,
            "elfClass": 64, "endianness": "little", "interpreter": None,
            "loadSegments": loads}


def encode_entry(name: str, ino: int, mode: int, nlink: int, data: bytes = b"",
                 rdevmajor: int = 0, rdevminor: int = 0) -> bytes:
    name_bytes = name.encode("ascii") + b"\0"
    fields = (ino, mode, 0, 0, nlink, FIXED_MTIME, len(data), 0, 0,
              rdevmajor, rdevminor, len(name_bytes), 0)
    header = MAGIC + b"".join(f"{field:08x}".encode("ascii") for field in fields)
    result = header + name_bytes
    result += b"\0" * (align4(len(result)) - len(result))
    result += data
    result += b"\0" * (align4(len(result)) - len(result))
    return result


def build(elf_path: pathlib.Path, archive_path: pathlib.Path,
          manifest_path: pathlib.Path) -> None:
    elf_info = parse_elf(elf_path)
    init_bytes = elf_path.read_bytes()
    entries = [
        (".", 1, 0o040755, 2, b"", 0, 0),
        ("dev", 2, 0o040755, 2, b"", 0, 0),
        ("dev/console", 3, 0o020600, 1, b"", 5, 1),
        ("dev/ttyS0", 4, 0o020600, 1, b"", 4, 64),
        ("init", 5, 0o100755, 1, init_bytes, 0, 0),
        (TRAILER, 6, 0, 1, b"", 0, 0),
    ]
    archive = b"".join(encode_entry(*entry) for entry in entries)
    archive_path.write_bytes(archive)
    parsed = validate_archive(archive)
    manifest = {
        "schemaVersion": 1,
        "phase": "RISCV-26",
        "format": "uncompressed newc (070701)",
        "archiveSha256": hashlib.sha256(archive).hexdigest(),
        "archiveBytes": len(archive),
        "fixedMtime": FIXED_MTIME,
        "entries": parsed,
        "initElf": elf_info,
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def validate_archive(archive: bytes) -> list:
    entries = []
    cursor = 0
    expected_inode = 1
    while cursor < len(archive):
        if cursor + 110 > len(archive) or archive[cursor:cursor + 6] != MAGIC:
            raise ValueError(f"invalid newc magic/header at offset {cursor}")
        values = [int(archive[cursor + 6 + index * 8:
                              cursor + 14 + index * 8], 16)
                  for index in range(13)]
        (ino, mode, uid, gid, nlink, mtime, filesize, devmajor, devminor,
         rdevmajor, rdevminor, namesize, check) = values
        if check != 0 or namesize < 1 or ino != expected_inode:
            raise ValueError("newc metadata or deterministic inode sequence is invalid")
        expected_inode += 1
        name_start = cursor + 110
        name_end = name_start + namesize
        if name_end > len(archive) or archive[name_end - 1] != 0:
            raise ValueError("newc pathname is truncated or lacks NUL terminator")
        name = archive[name_start:name_end - 1].decode("ascii")
        data_start = align4(name_end)
        data_end = data_start + filesize
        if data_end > len(archive):
            raise ValueError("newc file data is truncated")
        data = archive[data_start:data_end]
        cursor = align4(data_end)
        if cursor > len(archive):
            raise ValueError("newc data padding is truncated")
        entries.append({"name": name, "mode": mode, "uid": uid, "gid": gid,
                        "nlink": nlink, "mtime": mtime, "filesize": filesize,
                        "devmajor": devmajor, "devminor": devminor,
                        "rdevmajor": rdevmajor, "rdevminor": rdevminor,
                        "data": data})
        if name == TRAILER:
            if cursor != len(archive):
                raise ValueError("bytes follow the newc trailer")
            break
    if not entries or entries[-1]["name"] != TRAILER:
        raise ValueError("newc trailer is missing")
    expected = {".": (0o040755, b""), "dev": (0o040755, b""),
                "dev/console": (0o020600, b""),
                "dev/ttyS0": (0o020600, b""), "init": (0o100755, None),
                TRAILER: (0, b"")}
    if [entry["name"] for entry in entries] != list(expected):
        raise ValueError("initramfs contains unexpected or missing paths/order")
    for entry in entries:
        mode, expected_data = expected[entry["name"]]
        if entry["mode"] != mode or entry["uid"] != 0 or entry["gid"] != 0 or \
                entry["mtime"] != FIXED_MTIME:
            raise ValueError(f"unexpected metadata on {entry['name']}")
        if expected_data is not None and entry["data"] != expected_data:
            raise ValueError(f"unexpected data on {entry['name']}")
        if entry["name"] == "dev/console" and \
                (entry["rdevmajor"], entry["rdevminor"]) != (5, 1):
            raise ValueError("/dev/console must be character device 5:1")
        if entry["name"] == "dev/ttyS0" and \
                (entry["rdevmajor"], entry["rdevminor"]) != (4, 64):
            raise ValueError("/dev/ttyS0 must be character device 4:64")
    return [{key: value for key, value in entry.items() if key != "data"}
            for entry in entries]


def validate(archive_path: pathlib.Path, elf_path: pathlib.Path) -> None:
    archive = archive_path.read_bytes()
    entries = validate_archive(archive)
    elf = parse_elf(elf_path)
    init_entry = next(entry for entry in entries if entry["name"] == "init")
    if init_entry["filesize"] != elf["bytes"]:
        raise ValueError("CPIO /init size differs from independently inspected ELF")
    print(json.dumps({"valid": True, "archiveBytes": len(archive),
                      "archiveSha256": hashlib.sha256(archive).hexdigest(),
                      "entries": entries, "initElf": elf}, indent=2))


def main(argv: list[str]) -> None:
    if len(argv) == 5 and argv[1] == "build":
        build(pathlib.Path(argv[2]), pathlib.Path(argv[3]), pathlib.Path(argv[4]))
    elif len(argv) == 4 and argv[1] == "validate":
        validate(pathlib.Path(argv[2]), pathlib.Path(argv[3]))
    else:
        raise SystemExit("usage: build_initramfs.py build ELF CPIO MANIFEST | validate CPIO ELF")


if __name__ == "__main__":
    try:
        main(sys.argv)
    except (OSError, ValueError) as error:
        print(f"initramfs error: {error}", file=sys.stderr)
        raise SystemExit(1)
