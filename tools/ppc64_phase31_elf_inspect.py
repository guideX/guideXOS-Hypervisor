#!/usr/bin/env python3
"""Inspect the authenticated PPC64 Linux ELF around the Phase 31 TDI site."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


TARGET_NIP = 0xC000000001EAA2C8
EXPECTED_KERNEL_SHA256 = (
    "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd"
)


def c_string(data: bytes, offset: int) -> str:
    end = data.find(b"\0", offset)
    if end < 0:
        raise ValueError("unterminated ELF string")
    return data[offset:end].decode("utf-8", errors="replace")


def inspect(kernel: Path, config: Path) -> str:
    data = kernel.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != EXPECTED_KERNEL_SHA256:
        raise ValueError(f"unexpected kernel SHA-256: {digest}")
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise ValueError("input is not an ELF image")
    if data[4] != 2 or data[5] != 2:
        raise ValueError("expected ELF64 big-endian input")

    header = struct.unpack_from(">16sHHIQQQIHHHHHH", data, 0)
    _, elf_type, machine, _, entry, phoff, shoff, _, _, phentsize, phnum, \
        shentsize, shnum, shstrndx = header
    if elf_type != 2 or machine != 21:
        raise ValueError("expected PPC64 ET_EXEC kernel")

    raw_sections = [
        struct.unpack_from(">IIQQQQIIQQ", data, shoff + i * shentsize)
        for i in range(shnum)
    ]
    shstr = data[raw_sections[shstrndx][4]:
                 raw_sections[shstrndx][4] + raw_sections[shstrndx][5]]
    sections = []
    for index, section in enumerate(raw_sections):
        name = c_string(shstr, section[0]) if section[0] else ""
        sections.append({
            "index": index,
            "name": name,
            "type": section[1],
            "address": section[3],
            "offset": section[4],
            "size": section[5],
            "link": section[6],
            "entry_size": section[9],
        })

    string_tables = {
        section["index"]: data[section["offset"]:
            section["offset"] + section["size"]]
        for section in sections if section["type"] == 3
    }
    symbols = []
    for section in sections:
        if section["type"] != 2:
            continue
        strings = string_tables[section["link"]]
        entry_size = section["entry_size"] or 24
        for offset in range(section["offset"],
                            section["offset"] + section["size"], entry_size):
            name, info, other, shndx, value, size = struct.unpack_from(
                ">IBBHQQ", data, offset)
            symbol_name = c_string(strings, name) if name else ""
            if symbol_name:
                symbols.append((symbol_name, value, size, shndx))

    site_symbols = [symbol for symbol in symbols
                    if symbol[0] == "fixup_boot_paca"]
    if len(site_symbols) != 1:
        raise ValueError(f"expected one fixup_boot_paca symbol, found {len(site_symbols)}")
    symbol_name, symbol_value, symbol_size, symbol_section = site_symbols[0]
    if not (symbol_value <= TARGET_NIP < symbol_value + symbol_size):
        raise ValueError("target NIP is outside fixup_boot_paca")

    bug_section = next((section for section in sections
                        if section["name"] == "__bug_table"), None)
    if bug_section is None:
        raise ValueError("authenticated ELF has no __bug_table section")
    if bug_section["size"] % 12:
        raise ValueError("unexpected PowerPC DEBUG_BUGVERBOSE relative entry size")

    cfg = config.read_text(encoding="utf-8", errors="replace")
    for setting in ("CONFIG_BUG=y", "CONFIG_GENERIC_BUG=y",
                    "CONFIG_GENERIC_BUG_RELATIVE_POINTERS=y",
                    "CONFIG_DEBUG_BUGVERBOSE=y"):
        if setting not in cfg:
            raise ValueError(f"authenticated config does not contain {setting}")

    # PowerPC's relative bug entries store each address relative to the
    # address of that 32-bit field. With DEBUG_BUGVERBOSE the layout is:
    # bug_addr_disp, file_disp, line, flags (all big endian).
    matches = []
    bug_bytes = data[bug_section["offset"]:
                     bug_section["offset"] + bug_section["size"]]
    count = bug_section["size"] // 12
    for index in range(count):
        offset = index * 12
        bug_disp, file_disp, line, flags = struct.unpack_from(
            ">iiHH", bug_bytes, offset)
        bug_address = bug_section["address"] + offset + bug_disp
        if bug_address != TARGET_NIP:
            continue
        file_address = bug_section["address"] + offset + 4 + file_disp
        file_offset = None
        for phindex in range(phnum):
            ph = struct.unpack_from(">IIQQQQQQ", data,
                                    phoff + phindex * phentsize)
            p_type, _, p_offset, p_vaddr, _, p_filesz, _, _ = ph
            if p_type == 1 and p_vaddr <= file_address < p_vaddr + p_filesz:
                file_offset = p_offset + file_address - p_vaddr
                break
        if file_offset is None:
            raise ValueError("relative __bug_table file pointer is outside PT_LOAD")
        filename = c_string(data, file_offset)
        matches.append((index, line, flags, filename))

    if len(matches) != 1:
        raise ValueError(f"expected one bug-table entry at target, found {len(matches)}")

    match_index, line, flags, filename = matches[0]
    relative_symbol_offset = TARGET_NIP - symbol_value
    target_file_offset = None
    for phindex in range(phnum):
        ph = struct.unpack_from(">IIQQQQQQ", data,
                                phoff + phindex * phentsize)
        p_type, _, p_offset, p_vaddr, _, p_filesz, _, _ = ph
        if p_type == 1 and p_vaddr <= TARGET_NIP < p_vaddr + p_filesz:
            target_file_offset = p_offset + TARGET_NIP - p_vaddr
            break
    if target_file_offset is None:
        raise ValueError("target NIP is not backed by a PT_LOAD file range")
    target_word = struct.unpack_from(">I", data, target_file_offset)[0]
    surrounding = []
    for address in range(TARGET_NIP - 0x20, TARGET_NIP + 0x24, 4):
        for phindex in range(phnum):
            ph = struct.unpack_from(">IIQQQQQQ", data,
                                    phoff + phindex * phentsize)
            p_type, _, p_offset, p_vaddr, _, p_filesz, _, _ = ph
            if p_type == 1 and p_vaddr <= address <= p_vaddr + p_filesz - 4:
                offset = p_offset + address - p_vaddr
                word = struct.unpack_from(">I", data, offset)[0]
                surrounding.append(f"0x{address:016x}=0x{word:08x}")
                break

    classification = "WARN_ON" if flags & 1 else "BUG/other"
    return "\n".join([
        "result=PASS",
        f"kernel={kernel}",
        f"kernel_sha256={digest}",
        "elf=ELF64 big-endian EM_PPC64 ET_EXEC",
        f"entry=0x{entry:016x}",
        f"target_nip=0x{TARGET_NIP:016x}",
        f"target_word=0x{target_word:08x}",
        f"symbol={symbol_name}",
        f"symbol_value=0x{symbol_value:016x}",
        f"symbol_size={symbol_size}",
        f"symbol_section_index={symbol_section}",
        f"symbol_offset=0x{relative_symbol_offset:x}",
        f"bug_table_address=0x{bug_section['address']:016x}",
        f"bug_table_size=0x{bug_section['size']:x}",
        f"bug_table_entry_size=12",
        f"bug_table_entry_count={count}",
        f"bug_table_match_index={match_index}",
        f"bug_instruction_address=0x{TARGET_NIP:016x}",
        f"bug_file={filename}",
        f"bug_line={line}",
        f"bug_flags=0x{flags:04x}",
        f"bug_classification={classification}",
        "bug_table_relative_pointer_decoding=verified",
        "surrounding_words:",
        *surrounding,
    ]) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = inspect(args.kernel, args.config)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report, encoding="utf-8")
    print(report, end="")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # provide a concise test-friendly failure
        raise SystemExit(f"PPC64 Phase 31 ELF inspection failed: {error}")
