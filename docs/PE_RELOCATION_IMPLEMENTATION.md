# PE/COFF and IA-64 gnu-efi relocation ownership

## Current model

The PE parser maps an EFI image into its allocated `SizeOfImage` range and
applies only the PE/COFF base-relocation directory. For an IA-64 gnu-efi
image, the embedded ELF-style `.dynamic`, `.rela`, and `.dynsym` sections are
mapped as ordinary image data and deliberately left unmodified. The image's
gnu-efi startup routine calls `_relocate` and owns those IA-64 relocations.

This split is required for nonzero image bases. Applying `.rela` in both the
host loader and `_relocate` adds the load base twice. A preferred-base-zero
run hides that error because both additions are zero.

## PE/COFF `.reloc`

`applyRelocations()` computes:

```text
delta = loadAddress - ImageBase
```

When `delta` is nonzero, `applyPEBaseRelocations()` processes the PE
relocation blocks. A `DIR64` entry reads the mapped 64-bit target and writes
`target + delta`. These entries are firmware/PE-loader-owned. In the
authentic IA-64 image there are two entries, both for the entry PLABEL fields
(the code address and GP).

When `delta` is zero, PE relocation writes are unnecessary. A nonpreferred PE
image without `.reloc` is reported as lacking a relocation source; an
embedded IA-64 `.rela` stream is not used as a substitute by the PE loader.

## IA-64 gnu-efi `.dynamic` / `.rela`

The IA-64 gnu-efi `_start` routine passes the loaded image base and
`_DYNAMIC` to its `_relocate` routine. `_relocate` discovers `DT_RELA`,
`DT_RELASZ`, `DT_RELAENT`, and `DT_SYMTAB`, then applies the records in the
guest.

For `R_IA64_REL64LSB`, the matching gnu-efi IA-64 startup code performs:

```text
target = ImageBase + r_offset
*target = *target + ImageBase
```

The mapped target's linked value is therefore adjusted exactly once by the
guest. The authentic image has 227 such records.

For `R_IA64_FPTR64LSB`, the guest allocates descriptors from its static
`fptr_mem_base` area. Each 16-byte IA-64 descriptor contains the relocated
code address at offset 0 and GP at offset 8. The authentic image has 78
such records. The PE loader must not manufacture a second descriptor or
rewrite the FPTR target before `_relocate` runs.

## Authentic image evidence

The recovered `bootia64_exact.efi` has:

- preferred `ImageBase = 0` and `SizeOfImage = 0x5e000`;
- PE `.reloc` at RVA `0x5a000`, size `0xc`, with two `DIR64` entries;
- `.dynamic` at RVA `0x57000`;
- `DT_RELA = 0x58000`, `DT_RELASZ = 0x1c98`, `DT_RELAENT = 0x18`;
- 305 IA-64 `.rela` records: 227 `REL64LSB` and 78 `FPTR64LSB`;
- gnu-efi `_relocate` at image RVA `0x36d60`, including the REL64 and
  FPTR write loops.

The first causal record is `.rela` index 0: `r_offset = 0x3fad0`, type
`REL64LSB`, symbol index 0, addend `0x402b0`, and on-disk target value
`0x402b0`. With the repaired load base `0x1fd52000`, the host leaves that
value unchanged and the guest writes `0x1fd922b0`. The former host-plus-guest
path wrote it in the host first and the guest added the same base again,
producing `0x3fae42b0` and the invalid `init_devices` access at
`0x3fae42c8`.

## Regression requirements

Tests must keep the responsibilities separate:

1. PE `DIR64` targets change at a nonzero base.
2. Embedded IA-64 `.rela` target bytes remain linked/unrelocated after
   `PEParser::loadImage()` / `applyRelocations()`.
3. A guest `REL64LSB` application changes a representative datum once.
4. A guest `FPTR64LSB` application creates one 16-byte code/GP descriptor.
5. Initial EFI entry and ordinary `LoadImage` preserve the same model.
6. Zero-base loading remains compatible without being the only relocation
   test.

The legacy `applyELFRelocations()` helper remains available as non-PE/legacy
code reference, but it is not called by the PE/EFI load path.
