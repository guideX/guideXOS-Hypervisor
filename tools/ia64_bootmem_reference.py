#!/usr/bin/env python3
"""Read-only verifier for the captured Debian IA-64 Linux bootmem bitmap."""

from __future__ import annotations

import argparse
from pathlib import Path

NODE_LOW_PFN = 0x8000
BITMAP_BYTES = (NODE_LOW_PFN + 7) // 8

# PFN intervals observed in Linux 3.2.78 free_node_bootmem() callbacks.
FREE_RANGES = (
    (0x41, 0x1000),
    (0x135E, 0x7D13),
    (0x7F3E, 0x7F4C),
    (0x7F4F, 0x7FFC),
)

# init_bootmem_node() reserves the per-node structures and its own bitmap.
RESERVED_RANGES = (
    (0x80, 0x285),
    (0x285, 0x286),
)


def set_range(bitmap: bytearray, start: int, end: int, value: bool) -> None:
    if not (0 <= start <= end <= NODE_LOW_PFN):
        raise ValueError(f"PFN interval out of range: [{start:#x}, {end:#x})")
    for pfn in range(start, end):
        mask = 1 << (pfn & 7)
        if value:
            bitmap[pfn >> 3] |= mask
        else:
            bitmap[pfn >> 3] &= ~mask & 0xFF


def build_expected_bitmap() -> bytearray:
    expected = bytearray(b"\xff" * BITMAP_BYTES)
    for start, end in FREE_RANGES:
        set_range(expected, start, end, False)
    for start, end in RESERVED_RANGES:
        set_range(expected, start, end, True)
    return expected


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot", type=Path, help="captured guest memory range starting at 0xa14000")
    parser.add_argument("--bitmap-offset", type=lambda value: int(value, 0), default=0,
                        help="bitmap byte offset within the captured range")
    args = parser.parse_args()
    if args.bitmap_offset < 0:
        parser.error("--bitmap-offset must be nonnegative")

    captured = args.snapshot.read_bytes()
    end = args.bitmap_offset + BITMAP_BYTES
    if end > len(captured):
        parser.error(f"snapshot has {len(captured)} bytes; need [{args.bitmap_offset}, {end})")

    actual = captured[args.bitmap_offset:end]
    expected = build_expected_bitmap()
    mismatches = [
        pfn for pfn in range(NODE_LOW_PFN)
        if ((actual[pfn >> 3] ^ expected[pfn >> 3]) >> (pfn & 7)) & 1
    ]
    free_pfns = sum(byte.bit_count() for byte in (byte ^ 0xFF for byte in actual))
    print(
        f"snapshot={args.snapshot} capture_bytes={len(captured)} "
        f"bitmap_byte_range=[{args.bitmap_offset:#x},{end:#x}) "
        f"node_low_pfn={NODE_LOW_PFN:#x} bitmap_bytes={BITMAP_BYTES} "
        f"free_pfn_bits={free_pfns} mismatches={len(mismatches)}"
    )
    if mismatches:
        print("first_mismatched_pfns=" + ",".join(f"{pfn:#x}" for pfn in mismatches[:32]))
        raise SystemExit(1)
    print("bootmem_bitmap=PASS")


if __name__ == "__main__":
    main()
