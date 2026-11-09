#!/usr/bin/env python3
"""Read-only reference checker for Linux's 64-bit find_next_bit contract."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Iterable

WORD_BITS = 64
WORD_BYTES = WORD_BITS // 8
WORD_MASK = (1 << WORD_BITS) - 1


def find_next_set_bit(bitmap: bytes, size: int, offset: int) -> tuple[int, list[tuple[int, int]]]:
    """Return the next set bit or size, reading only bytes for declared bits.

    Bitmap words use the little-endian 64-bit representation of the target
    IA-64 kernel. The returned word list contains (word index, value) pairs
    actually consulted by the reference search.
    """
    if size < 0 or offset < 0:
        raise ValueError("size and offset must be nonnegative")
    if offset >= size:
        return size, []

    declared_bytes = (size + 7) // 8
    if len(bitmap) < declared_bytes:
        raise ValueError(
            f"bitmap has {len(bitmap)} bytes, but {size} bits require "
            f"{declared_bytes} bytes"
        )

    result = (offset // WORD_BITS) * WORD_BITS
    word_index = offset // WORD_BITS
    bit_offset = offset % WORD_BITS
    words: list[tuple[int, int]] = []

    while result < size:
        word_start_bit = word_index * WORD_BITS
        valid_bits = min(WORD_BITS, size - word_start_bit)
        byte_start = word_index * WORD_BYTES
        byte_count = (valid_bits + 7) // 8
        word = int.from_bytes(bitmap[byte_start : byte_start + byte_count], "little")
        words.append((word_index, word))

        if word_index == offset // WORD_BITS:
            word &= WORD_MASK ^ ((1 << bit_offset) - 1)
        word &= (1 << valid_bits) - 1
        if word:
            low_bit = (word & -word).bit_length() - 1
            return word_start_bit + low_bit, words

        word_index += 1
        result = word_index * WORD_BITS

    return size, words


def run_vectors() -> None:
    vectors: Iterable[tuple[str, bytes, int, int, int]] = (
        ("offset-zero-first-bit", bytes.fromhex("01"), 8, 0, 0),
        ("offset-at-size", b"", 0, 0, 0),
        ("offset-past-size", b"", 8, 8, 8),
        ("word-boundary", (1 << 64).to_bytes(16, "little"), 128, 64, 64),
        ("inside-word", ((1 << 62) | (1 << 63) | (1 << 64)).to_bytes(16, "little"), 128, 63, 63),
        ("last-partial-word", (1 << 129).to_bytes(17, "little"), 130, 128, 129),
        ("no-set-bit-in-range", (1 << 3).to_bytes(1, "little"), 8, 4, 8),
        ("first-at-offset", (1 << 65).to_bytes(16, "little"), 128, 65, 65),
        ("last-valid-position", (1 << 126).to_bytes(16, "little"), 127, 64, 126),
        # Bits 130..135 are physically present in the last byte but outside size.
        ("mask-outside-size", ((1 << 135) | (1 << 129)).to_bytes(17, "little"), 130, 0, 129),
    )
    for name, bitmap, size, offset, expected in vectors:
        observed, _ = find_next_set_bit(bitmap, size, offset)
        if observed != expected:
            raise AssertionError(f"{name}: expected {expected}, got {observed}")
        print(f"PASS {name}: result={observed}")
    print(f"PASS synthetic-vectors: {len(tuple(vectors))}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bitmap", nargs="?", type=Path)
    parser.add_argument("--size", type=int, help="declared bitmap size in bits")
    parser.add_argument("--offset", type=lambda value: int(value, 0), help="starting bit offset")
    parser.add_argument("--bitmap-offset", type=lambda value: int(value, 0), default=0,
                        help="byte offset of the bitmap inside the captured memory range")
    parser.add_argument("--self-test", action="store_true", help="run focused boundary vectors")
    args = parser.parse_args()

    if args.self_test:
        run_vectors()
    if args.bitmap is None:
        if args.size is not None or args.offset is not None:
            parser.error("bitmap path is required when size/offset are supplied")
        return
    if args.size is None or args.offset is None:
        parser.error("--size and --offset are required with a bitmap path")
    if args.bitmap_offset < 0:
        parser.error("--bitmap-offset must be nonnegative")

    captured = args.bitmap.read_bytes()
    needed = (args.size + 7) // 8
    end = args.bitmap_offset + needed
    if end > len(captured):
        parser.error(
            f"capture has {len(captured)} bytes; bitmap range "
            f"[{args.bitmap_offset}, {end}) is unavailable"
        )
    bitmap = captured[args.bitmap_offset:end]
    result, words = find_next_set_bit(bitmap, args.size, args.offset)
    print(
        f"bitmap={args.bitmap} capture_bytes={len(captured)} "
        f"bitmap_byte_range=[{args.bitmap_offset},{end}) "
        f"declared_bits={args.size} offset={args.offset} "
        f"bitmap_hex={bitmap.hex()}"
    )
    for index, value in words:
        print(f"word[{index}]_little_endian=0x{value:016x}")
    print(f"reference_result={result}")


if __name__ == "__main__":
    main()
