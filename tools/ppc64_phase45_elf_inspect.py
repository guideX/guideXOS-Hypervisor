#!/usr/bin/env python3
"""Independently decode the authenticated POWER9 MMCR1 Linux frontier."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


EXPECTED_KERNEL_SHA256 = (
    "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd"
)
MMCR1_NIP = 0xC00000000003AD30
MMCR1_WORD = 0x7D3EC3A6
NEXT_NIP = 0xC00000000003AD34
NEXT_WORD = 0x7D31C3A6


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


def inspect(kernel: Path) -> str:
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
    symbols: list[tuple[str, int, int, int]] = []
    for section in sections:
        if section[1] != 2:
            continue
        strings = string_tables[section[6]]
        entry_size = section[9] or 24
        for offset in range(section[4], section[4] + section[5], entry_size):
            name, info, _, _, value, size = struct.unpack_from(
                ">IBBHQQ", data, offset)
            if name and (info & 0xF) == 2 and size:
                symbols.append((c_string(strings, name), value, size, info & 0xF))

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

    word = word_at(MMCR1_NIP)
    next_word = word_at(NEXT_NIP)
    decoded = decode_mtspr(word)
    next_decoded = decode_mtspr(next_word)
    if (word, *decoded, word & 1) != (MMCR1_WORD, 31, 9, 30, 24, 798, 467, 0):
        raise ValueError(f"unexpected MMCR1 instruction {word:#010x}")
    if (next_word, *next_decoded, next_word & 1) != \
            (NEXT_WORD, 31, 9, 17, 24, 785, 467, 0):
        raise ValueError(f"unexpected next instruction {next_word:#010x}")

    containing = [item for item in symbols
                  if item[1] <= MMCR1_NIP < item[1] + item[2]]
    containing.sort(key=lambda item: (item[1], item[2]))
    owner = containing[-1] if containing else None
    if owner is None or owner[0] != "__setup_cpu_power9":
        raise ValueError("MMCR1 frontier is not owned by __setup_cpu_power9")

    lines = [
        "result=PASS",
        f"kernel={kernel}",
        f"kernel_sha256={digest}",
        "elf=ELF64 big-endian EM_PPC64 ET_EXEC",
        f"entry=0x{entry:016x}",
        f"mmcr1_nip=0x{MMCR1_NIP:016x}",
        f"mmcr1_word=0x{word:08x}",
        f"primary={decoded[0]}",
        f"RS=r{decoded[1]}",
        f"spr_low5={decoded[2]}",
        f"spr_high5={decoded[3]}",
        f"spr={decoded[2]}|({decoded[3]}<<5)={decoded[4]}",
        f"XO={decoded[5]}",
        f"reserved_final_bit={word & 1}",
        f"mmcr1_file_offset=0x{file_offset(MMCR1_NIP):x}",
        f"containing_symbol={owner[0]}",
        f"symbol_address=0x{owner[1]:016x}",
        f"symbol_size=0x{owner[2]:x}",
        f"symbol_offset=0x{MMCR1_NIP - owner[1]:x}",
        f"next_nip=0x{NEXT_NIP:016x}",
        f"next_word=0x{next_word:08x}",
        f"next_RS=r{next_decoded[1]}",
        f"next_spr={next_decoded[4]}",
        f"next_XO={next_decoded[5]}",
        "source_path=__setup_cpu_power9 -> init_PMU: MMCRA, MMCR0, MMCR1, MMCR2",
        "instruction_window_begin",
    ]
    for address in range(MMCR1_NIP - 8, MMCR1_NIP + 16, 4):
        lines.append(f"0x{address:016x}: 0x{word_at(address):08x}")
    lines.append("instruction_window_end")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = inspect(args.kernel)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report, encoding="utf-8")
    print(report, end="")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        raise SystemExit(f"PPC64 Phase 45 ELF inspection failed: {error}")
