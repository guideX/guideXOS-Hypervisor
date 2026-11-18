#!/usr/bin/env python3
"""Build and independently validate the deterministic RISCV-28 newc archive."""

import hashlib
import json
import pathlib
import struct
import sys


MAGIC = b"070701"
TRAILER = "TRAILER!!!"


def align4(value: int) -> int:
    return (value + 3) & ~3


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_elf(path: pathlib.Path, expected_entry: int) -> dict:
    data = path.read_bytes()
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4:6] != b"\x02\x01":
        raise ValueError(f"{path.name} must be ELF64 little-endian")
    if data[6] != 1 or data[7:9] != b"\0\0":
        raise ValueError(f"{path.name} must use the System V ELF ABI")
    (etype, machine, version, entry, phoff, shoff, flags, ehsize,
     phentsize, phnum, shentsize, shnum, shstrndx) = struct.unpack_from(
         "<HHIQQQIHHHHHH", data, 16)
    del shoff, shentsize, shnum, shstrndx
    if (etype, machine, version, entry, ehsize, phentsize) != (2, 243, 1,
            expected_entry, 64, 56):
        raise ValueError(f"{path.name} must be a static RV64 ET_EXEC at {expected_entry:#x}")
    if flags & 1:
        raise ValueError(f"{path.name} advertises the compressed instruction ABI")
    if phoff > len(data) or phnum > (len(data) - phoff) // phentsize:
        raise ValueError(f"{path.name} program-header table is truncated")

    loads = []
    has_interp = False
    has_dynamic = False
    entry_executable = False
    for index in range(phnum):
        (p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz,
         p_memsz, p_align) = struct.unpack_from("<IIQQQQQQ", data,
                                                phoff + index * phentsize)
        del p_paddr
        if p_type == 3:
            has_interp = True
        if p_type == 2:
            has_dynamic = True
        if p_type != 1:
            continue
        if p_filesz > p_memsz or p_offset > len(data) or p_filesz > len(data) - p_offset:
            raise ValueError(f"{path.name} has a truncated PT_LOAD extent")
        if p_align not in (0, 1):
            if p_align & (p_align - 1) or (p_vaddr - p_offset) % p_align:
                raise ValueError(f"{path.name} has invalid PT_LOAD alignment")
        if p_align >= 4096 and (p_vaddr % 4096) != (p_offset % 4096):
            raise ValueError(f"{path.name} violates page-offset congruence")
        if p_flags & 1 and p_vaddr <= entry < p_vaddr + p_filesz:
            entry_executable = True
        loads.append({"offset": p_offset, "vaddr": p_vaddr, "filesz": p_filesz,
                      "memsz": p_memsz, "flags": p_flags, "align": p_align})
    if has_interp or has_dynamic or not loads:
        raise ValueError(f"{path.name} must not use PT_INTERP or PT_DYNAMIC")
    if not entry_executable:
        raise ValueError(f"{path.name} entrypoint is not file-backed executable PT_LOAD memory")
    for left, right in zip(loads, loads[1:]):
        if left["vaddr"] + left["memsz"] > right["vaddr"]:
            raise ValueError(f"{path.name} PT_LOAD memory ranges overlap")
    return {"sha256": sha256(data), "bytes": len(data), "entry": entry,
            "type": etype, "machine": machine, "elfClass": 64,
            "endianness": "little", "interpreter": None, "dynamic": False,
            "loadSegments": loads}


def encode_entry(name: str, ino: int, mode: int, nlink: int, data: bytes = b"",
                 rdevmajor: int = 0, rdevminor: int = 0) -> bytes:
    name_bytes = name.encode("ascii") + b"\0"
    fields = (ino, mode, 0, 0, nlink, 0, len(data), 0, 0,
              rdevmajor, rdevminor, len(name_bytes), 0)
    header = MAGIC + b"".join(f"{field:08x}".encode("ascii") for field in fields)
    result = header + name_bytes
    result += b"\0" * (align4(len(result)) - len(result))
    result += data
    result += b"\0" * (align4(len(result)) - len(result))
    return result


def parse_archive(data: bytes) -> list[dict]:
    entries = []
    cursor = 0
    while cursor < len(data):
        if cursor + 110 > len(data) or data[cursor:cursor + 6] != MAGIC:
            raise ValueError(f"invalid newc header at {cursor}")
        fields = [int(data[cursor + 6 + i * 8:cursor + 14 + i * 8], 16)
                  for i in range(13)]
        (ino, mode, uid, gid, nlink, mtime, filesize, devmajor, devminor,
         rdevmajor, rdevminor, namesize, check) = fields
        name_start = cursor + 110
        name_end = name_start + namesize
        if check or namesize < 1 or name_end > len(data) or data[name_end - 1] != 0:
            raise ValueError("invalid newc metadata or pathname")
        name = data[name_start:name_end - 1].decode("ascii")
        data_start = align4(name_end)
        data_end = data_start + filesize
        next_cursor = align4(data_end)
        if data_end > len(data) or next_cursor > len(data):
            raise ValueError(f"truncated newc entry {name}")
        if ino != len(entries) + 1 or uid or gid or mtime:
            raise ValueError(f"non-deterministic metadata on {name}")
        entries.append({"name": name, "inode": ino, "mode": mode,
                        "uid": uid, "gid": gid, "nlink": nlink, "mtime": mtime,
                        "filesize": filesize, "devmajor": devmajor,
                        "devminor": devminor, "rdevmajor": rdevmajor,
                        "rdevminor": rdevminor, "data": data[data_start:data_end]})
        cursor = next_cursor
        if name == TRAILER:
            if cursor != len(data):
                raise ValueError("bytes follow the newc trailer")
            break
    expected = [".", "bin", "dev", "dev/console", "dev/ttyS0", "init",
                "bin/child", TRAILER]
    if [entry["name"] for entry in entries] != expected:
        raise ValueError("initramfs paths are missing, reordered, or unexpected")
    expected_modes = [0o040755, 0o040755, 0o040755, 0o020600, 0o020600,
                      0o100755, 0o100755, 0]
    for entry, mode in zip(entries, expected_modes):
        if entry["mode"] != mode:
            raise ValueError(f"unexpected file mode for {entry['name']}")
    if (entries[3]["rdevmajor"], entries[3]["rdevminor"]) != (5, 1):
        raise ValueError("/dev/console must be character device 5:1")
    if (entries[4]["rdevmajor"], entries[4]["rdevminor"]) != (4, 64):
        raise ValueError("/dev/ttyS0 must be character device 4:64")
    return entries


def build(parent: pathlib.Path, child: pathlib.Path, archive_path: pathlib.Path,
          manifest_path: pathlib.Path) -> None:
    parent_info = parse_elf(parent, 0x10000)
    child_info = parse_elf(child, 0x20000)
    entries = [
        (".", 1, 0o040755, 2, b"", 0, 0),
        ("bin", 2, 0o040755, 2, b"", 0, 0),
        ("dev", 3, 0o040755, 2, b"", 0, 0),
        ("dev/console", 4, 0o020600, 1, b"", 5, 1),
        ("dev/ttyS0", 5, 0o020600, 1, b"", 4, 64),
        ("init", 6, 0o100755, 1, parent.read_bytes(), 0, 0),
        ("bin/child", 7, 0o100755, 1, child.read_bytes(), 0, 0),
        (TRAILER, 8, 0, 1, b"", 0, 0),
    ]
    archive = b"".join(encode_entry(*entry) for entry in entries)
    archive_path.write_bytes(archive)
    parsed = parse_archive(archive)
    manifest = {
        "schemaVersion": 1,
        "phase": "RISCV-28",
        "format": "uncompressed newc (070701)",
        "archiveSha256": sha256(archive),
        "archiveBytes": len(archive),
        "fixedMtime": 0,
        "entries": [{key: value for key, value in item.items() if key != "data"}
                    for item in parsed],
        "parentElf": parent_info,
        "childElf": child_info,
        "parentElfSha256": parent_info["sha256"],
        "childElfSha256": child_info["sha256"],
        "processProof": {
            "syscalls": {"getpid": 172, "getppid": 173, "clone": 220,
                         "execve": 221, "wait4": 260, "write": 64,
                         "exit": 93},
            "cloneFlags": 17,
            "execvePath": "/bin/child",
            "argv": ["/bin/child", "execve-riscv28"],
            "envp": ["GUIDEXOS_RISCV28=present"],
            "childExitCode": 42,
            "waitStatus": 10752,
        },
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    manifest_path.with_name("parent-elf.json").write_text(
        json.dumps(parent_info, indent=2) + "\n", encoding="utf-8")
    manifest_path.with_name("child-elf.json").write_text(
        json.dumps(child_info, indent=2) + "\n", encoding="utf-8")


def main(argv: list[str]) -> None:
    if len(argv) == 6 and argv[1] == "build":
        build(pathlib.Path(argv[2]), pathlib.Path(argv[3]), pathlib.Path(argv[4]),
              pathlib.Path(argv[5]))
    elif len(argv) == 3 and argv[1] == "validate":
        parsed = parse_archive(pathlib.Path(argv[2]).read_bytes())
        init = next(item for item in parsed if item["name"] == "init")
        child = next(item for item in parsed if item["name"] == "bin/child")
        print(json.dumps({"valid": True, "archiveBytes": pathlib.Path(argv[2]).stat().st_size,
                          "entries": [{key: value for key, value in item.items()
                                       if key != "data"} for item in parsed],
                          "initElf": parse_elf_bytes(init["data"], 0x10000),
                          "childElf": parse_elf_bytes(child["data"], 0x20000)}, indent=2))
    else:
        raise SystemExit("usage: build_execve_initramfs.py build INIT CHILD CPIO MANIFEST | validate CPIO")


def parse_elf_bytes(data: bytes, expected_entry: int) -> dict:
    temporary = None
    # Keep validation logic shared with build-time file inspection.
    import tempfile
    with tempfile.NamedTemporaryFile(prefix="riscv28-", suffix=".elf", delete=False) as file:
        temporary = pathlib.Path(file.name)
        file.write(data)
    try:
        return parse_elf(temporary, expected_entry)
    finally:
        temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    try:
        main(sys.argv)
    except (OSError, ValueError, struct.error) as error:
        print(f"RISCV-28 initramfs error: {error}", file=sys.stderr)
        raise SystemExit(1)
