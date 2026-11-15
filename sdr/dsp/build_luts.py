#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Piotr Krysik
# SPDX-License-Identifier: GPL-3.0-only
"""Extracts the receiver gain / analog-filter tables of the RF chip from the firmware image into wtr_luts.bin.

  build_luts.py --nm IMAGE.nm --elf FIRMWARE.elf --out wtr_luts.bin

Each table (one per band, filter bandwidth and RF port) holds six rows of 27 register values: one row per gain state.
Writing a row into the chip's registers 0x045E..0x0478 and then 1 into the latch register 0x0479 selects that gain state
and that analog filter bandwidth.

File format: "WLUT", u32 count, then per table {char name[40] (e.g. "b8_bw10m_plb2"); u8 data[162]}."""

import argparse, re, struct, sys
from pathlib import Path

PREFIX, SUFFIX, SIZE = 'wtr4905_ltefdd_prxlgy1_', '_ag_gain_lut', 162


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nm', required=True)
    ap.add_argument('--elf', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    elf = Path(a.elf).read_bytes()
    if elf[:4] != b'\x7fELF' or elf[4] != 1:
        sys.exit('not a 32-bit ELF file: ' + a.elf)
    (phoff,) = struct.unpack_from('<I', elf, 0x1C)
    phentsize, phnum = struct.unpack_from('<HH', elf, 0x2A)
    segs = []
    for i in range(phnum):
        ptype, off, vaddr, paddr, filesz, memsz, flags, align = struct.unpack_from('<8I', elf, phoff + i * phentsize)
        if ptype == 1 and filesz:
            segs.append((vaddr, off, filesz))

    def read_va(va, n):
        for v, off, fs in segs:
            if v <= va and va + n <= v + fs:
                return elf[off + va - v : off + va - v + n]
        sys.exit('address 0x%x is not in the image' % va)

    tables = []
    for line in Path(a.nm).open(errors='ignore'):
        p = line.split()
        if len(p) == 4 and p[3].startswith(PREFIX) and p[3].endswith(SUFFIX) and int(p[1], 16) == SIZE:
            name = p[3][len(PREFIX) : -len(SUFFIX)]
            if re.fullmatch(r'b\d+_bw\w+_p\w+', name):
                tables.append((name, read_va(int(p[0], 16), SIZE)))
    if not tables:
        sys.exit('no gain tables found in %s' % a.nm)
    tables.sort()
    out = b'WLUT' + struct.pack('<I', len(tables)) + b''.join(n.encode().ljust(40, b'\0') + d for n, d in tables)
    Path(a.out).write_bytes(out)
    print('wrote %s: %d tables' % (a.out, len(tables)))


if __name__ == '__main__':
    main()
