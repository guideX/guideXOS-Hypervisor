#!/usr/bin/env python3
"""Independently verify the authenticated POWER9 MMCR2 frontier in Linux ELF."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


EXPECTED_KERNEL_SHA256 = (
    "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd"
)
MMCR2_NIP = 0xC00000000003AD34
MMCR2_WORD = 0x7D31C3A6


def c_string(data: bytes, offset: int) -> str:
    end = data.find(b"\0", offset)
    if end < 0:
        raise ValueError("unterminated ELF string")
    return data[offset:end].decode("utf-8", errors="replace")


def decode_mtspr(word: int) -> tuple[int, int, int, int, int, int]:
    primary = (word >> 26) & 0x3F
    rs = (word >> 21) & 0x1F
    low = (word >> 16) & 0x1F
    high = (word >> 11) & 0x1F
    spr = low | (high << 5)
    xo = (word >> 1) & 0x3FF
    return primary, rs, low, high, spr, xo


def decode_cntlzd(word: int) -> str | None:
    primary = (word >> 26) & 0x3F
    rs = (word >> 21) & 0x1F
    ra = (word >> 16) & 0x1F
    reserved = (word >> 11) & 0x1F
    xo = (word >> 1) & 0x3FF
    rc = word & 1
    if primary == 31 and xo == 58 and reserved == 0 and rc == 0:
        return f"cntlzd r{ra},r{rs}"
    return None


def inspect(kernel: Path, frontier_nip: int | None) -> str:
    data = kernel.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != EXPECTED_KERNEL_SHA256:
        raise ValueError(f"unexpected kernel SHA-256: {digest}")
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x02":
        raise ValueError("expected ELF64 big-endian image")

    header = struct.unpack_from(">16sHHIQQQIHHHHHH", data, 0)
    _, elf_type, machine, _, entry, phoff, shoff, _, _, phentsize, phnum, \
        shentsize, shnum, _ = header
    if elf_type != 2 or machine != 21:
        raise ValueError("expected PPC64 ET_EXEC kernel")

    sections = [
        struct.unpack_from(">IIQQQQIIQQ", data, shoff + i * shentsize)
        for i in range(shnum)
    ]
    string_tables = {
        index: data[section[4]:section[4] + section[5]]
        for index, section in enumerate(sections) if section[1] == 3
    }
    symbols: list[tuple[str, int, int]] = []
    for section in sections:
        if section[1] != 2:
            continue
        strings = string_tables[section[6]]
        entry_size = section[9] or 24
        for offset in range(section[4], section[4] + section[5], entry_size):
            name, info, _, _, value, size = struct.unpack_from(
                ">IBBHQQ", data, offset)
            if name and (info & 0xF) == 2 and size:
                symbols.append((c_string(strings, name), value, size))

    loads = []
    for index in range(phnum):
        ph = struct.unpack_from(">IIQQQQQQ", data, phoff + index * phentsize)
        if ph[0] == 1:
            loads.append(ph)

    def file_offset(address: int, width: int = 4) -> int:
        for _, _, offset, virtual, _, file_size, _, _ in loads:
            if virtual <= address and address + width <= virtual + file_size:
                return offset + address - virtual
        raise ValueError(f"address {address:#x} is not file-backed")

    def word_at(address: int) -> int:
        return struct.unpack_from(">I", data, file_offset(address))[0]

    def containing(address: int) -> tuple[str, int, int] | None:
        matches = [item for item in symbols
                   if item[1] <= address < item[1] + item[2]]
        matches.sort(key=lambda item: (item[1], item[2]))
        return matches[-1] if matches else None

    word = word_at(MMCR2_NIP)
    decoded = decode_mtspr(word)
    if (word, *decoded, word & 1) != (MMCR2_WORD, 31, 9, 17, 24, 785, 467, 0):
        raise ValueError(f"unexpected MMCR2 instruction {word:#010x}")
    owner = containing(MMCR2_NIP)
    if owner is None or owner[0] != "__setup_cpu_power9":
        raise ValueError("MMCR2 frontier is not owned by __setup_cpu_power9")

    lines = [
        "result=PASS",
        f"kernel={kernel}",
        f"kernel_sha256={digest}",
        "elf=ELF64 big-endian EM_PPC64 ET_EXEC",
        f"entry=0x{entry:016x}",
        f"mmcr2_nip=0x{MMCR2_NIP:016x}",
        f"mmcr2_word=0x{word:08x}",
        f"primary={decoded[0]}",
        f"RS=r{decoded[1]}",
        f"spr_low5={decoded[2]}",
        f"spr_high5={decoded[3]}",
        f"spr={decoded[2]}|({decoded[3]}<<5)={decoded[4]}",
        f"XO={decoded[5]}",
        f"reserved_final_bit={word & 1}",
        f"file_offset=0x{file_offset(MMCR2_NIP):x}",
        f"containing_symbol={owner[0]}",
        f"symbol_address=0x{owner[1]:016x}",
        f"symbol_size=0x{owner[2]:x}",
        f"symbol_offset=0x{MMCR2_NIP - owner[1]:x}",
        f"next_word=0x{word_at(MMCR2_NIP + 4):08x}",
        "source_path=__setup_cpu_power9 -> init_PMU: MMCRA, MMCR0, MMCR1, MMCR2",
    ]
    lines.append("instruction_window_begin")
    for address in range(MMCR2_NIP - 8, MMCR2_NIP + 12, 4):
        lines.append(f"0x{address:016x}: 0x{word_at(address):08x}")
    lines.append("instruction_window_end")

    if frontier_nip is not None:
        frontier_word = word_at(frontier_nip)
        frontier_owner = containing(frontier_nip)
        frontier_decode = decode_cntlzd(frontier_word)
        lines.extend([
            f"next_frontier_nip=0x{frontier_nip:016x}",
            f"next_frontier_word=0x{frontier_word:08x}",
            f"next_frontier_primary={(frontier_word >> 26) & 0x3f}",
            f"next_frontier_XO={(frontier_word >> 1) & 0x3ff}",
            "next_frontier_decode=" +
                (frontier_decode if frontier_decode else "unrecognized"),
            "next_frontier_symbol=" +
                (frontier_owner[0] if frontier_owner else "unattributed"),
            "next_frontier_symbol_address=" +
                (f"0x{frontier_owner[1]:016x}" if frontier_owner else "none"),
            "next_frontier_symbol_offset=" +
                (f"0x{frontier_nip - frontier_owner[1]:x}"
                 if frontier_owner else "none"),
        ])
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--frontier-nip", type=lambda value: int(value, 0))
    args = parser.parse_args()
    report = inspect(args.kernel, args.frontier_nip)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report, encoding="utf-8")
    print(report, end="")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        raise SystemExit(f"PPC64 Phase 46 ELF inspection failed: {error}")
