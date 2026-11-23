#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Piotr Krysik
# SPDX-License-Identifier: GPL-3.0-only
"""Builds the DSP programs of the sample stream into one file, dsp_blobs.bin, for the firmware image described by an
`nm` listing.

  build_blobs.py --tools HEXAGON_DIR --nm IMAGE.nm --fw-sha256sums PIL_DIR/SHA256SUMS --out dsp_blobs.bin

HEXAGON_DIR is the Hexagon tools directory that contains gnu/bin (for example .../6.4.06.a).
IMAGE.nm is `hexagon-nm -S <firmware .elf>`. Each program is linked at the addresses that `IMAGE.nm` gives for the
firmware functions it calls and for the sandbox scratch area, so the result is valid only for that exact firmware
build: a firmware image built from different sources (even a one-line change far from any of these addresses) can
shift where they end up, and a stale dsp_blobs.bin then calls the wrong code instead of failing loudly (this one bit
a real session: see ../README.md, "Keeping dsp_blobs.bin matched to the firmware").

PIL_DIR/SHA256SUMS is the file `../../firmware/build.sh` writes next to the firmware image it just built
(`sha256sum modem.* > SHA256SUMS`); its content fingerprints that exact build. sdrd checks the same file on the
phone (where it is installed by install_firmware.sh) against the fingerprint recorded here, and refuses to run a
blob built for different firmware instead of silently calling the wrong address.

File format "SDR2": "SDR2", u32 count, u64 firmware fingerprint, then per program {char name[16]; u32 entry; u32
size; u32 offset}, then the code bytes."""

import argparse, re, struct, subprocess, sys, tempfile
from pathlib import Path

FNV1A64_OFFSET, FNV1A64_PRIME, MASK64 = 0xCBF29CE484222325, 0x100000001B3, (1 << 64) - 1


def fnv1a64(data: bytes) -> int:
    h = FNV1A64_OFFSET
    for b in data:
        h = ((h ^ b) * FNV1A64_PRIME) & MASK64
    return h

HERE = Path(__file__).resolve().parent
PROGRAMS = [
    ('reg_read', 'reg_read.s', [], False),
    ('reg_write', 'reg_write.c', [], False),
    ('mem_alloc', 'mem_alloc.s', [], False),
    ('nv_bands', 'nv_bands.c', [], False),
    ('ccs_replay', 'ccs_replay.s', [], False),
    ('mcdma', 'mcdma.c', ['MC_QTAIL', 'MC_ENQ'], False),
    # optional: left out when the firmware image has no such routine (the driver then uses its own formula)
    ('pll_calc', 'pll_calc.c', [], True),
]
SCRATCH_SYMBOL, GP_SYMBOL = 'rf_sandbox_dyn_scratch', '_SDA_BASE_'
QTAIL_SYMBOL, ENQ_SYMBOL = '@MODEMFW_351565538', '@MODEMFW_344953589'
MAX_CODE = 0x800


def load_symbols(nm_file):
    sym = {}
    for line in Path(nm_file).open(errors='ignore'):
        p = line.split()
        if len(p) in (3, 4):
            try:
                sym.setdefault(p[-1], int(p[0], 16))
            except ValueError:
                pass
    return sym


def run(cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode:
        sys.exit('%s failed:\n%s' % (cmd[0], r.stderr[-1200:]))
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tools', required=True)
    ap.add_argument('--nm', required=True)
    ap.add_argument('--fw-sha256sums', required=True, metavar='FILE', help='SHA256SUMS written by firmware/build.sh for the same image as --nm')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    hb = Path(a.tools) / 'gnu/bin'
    sym = load_symbols(a.nm)
    fingerprint = fnv1a64(Path(a.fw_sha256sums).read_bytes())
    for need in (SCRATCH_SYMBOL, GP_SYMBOL, QTAIL_SYMBOL, ENQ_SYMBOL):
        if need not in sym:
            sys.exit('symbol %s not found in %s' % (need, a.nm))
    scratch = sym[SCRATCH_SYMBOL]
    defines = {'MC_QTAIL': '0x%xu' % (sym[QTAIL_SYMBOL] + 4), 'MC_ENQ': '0x%xu' % sym[ENQ_SYMBOL]}
    blobs = []
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        for name, src, needs, optional in PROGRAMS:
            o, elf, binf = td / (name + '.o'), td / (name + '.elf'), td / (name + '.bin')
            if src.endswith('.c'):
                run(
                    [
                        hb / 'hexagon-gcc',
                        '-mv5',
                        '-std=gnu99',
                        '-O2',
                        '-G0',
                        '-fno-pic',
                        '-ffreestanding',
                        *['-D%s=%s' % (n, defines[n]) for n in needs],
                        '-c',
                        HERE / src,
                        '-o',
                        o,
                    ]
                )
            else:
                run([hb / 'hexagon-as', '-mv5', '-o', o, HERE / src])
            undefined = [l.split()[-1] for l in run([hb / 'hexagon-nm', o]).splitlines() if re.match(r'^\s+U ', l)]
            defs = []
            missing = [u for u in undefined if u not in sym]
            if missing and optional:
                print('%-10s skipped (%s is not in the firmware image)' % (name, missing[0][:40]))
                continue
            for u in undefined:
                if u not in sym:
                    sys.exit('%s: undefined symbol %s is not in the firmware image' % (name, u))
                defs += ['--defsym', '%s=0x%x' % (u, sym[u])]
            defs += ['--defsym', '%s=0x%x' % (GP_SYMBOL, sym[GP_SYMBOL])]
            (td / 'l.ld').write_text(
                'SECTIONS { .text 0x%08x : { *(.text.entry) *(.text) *(.text.*) *(.rodata*) *(.sdata*) } }\n' % scratch
            )
            run([hb / 'hexagon-ld', '-T', td / 'l.ld', *defs, '-o', elf, o])
            run([hb / 'hexagon-objcopy', '-O', 'binary', elf, binf])
            code = binf.read_bytes()
            if len(code) > MAX_CODE:
                sys.exit('%s: %d bytes does not fit the %d-byte scratch area' % (name, len(code), MAX_CODE))
            m = re.search(r'^([0-9a-f]{8}) [Tt] fn$', run([hb / 'hexagon-nm', elf]), re.M)
            if not m:
                sys.exit('%s: entry symbol fn not found' % name)
            blobs.append((name, int(m.group(1), 16), code))
            print('%-10s entry 0x%08x  %4d bytes' % (name, blobs[-1][1], len(code)))
    table = 16 + 28 * len(blobs)
    off = table
    out = bytearray(b'SDR2' + struct.pack('<IQ', len(blobs), fingerprint))
    for name, entry, code in blobs:
        out += name.encode().ljust(16, b'\0') + struct.pack('<III', entry, len(code), off)
        off += len(code)
    for _, _, code in blobs:
        out += code
    Path(a.out).write_bytes(bytes(out))
    print('wrote %s (%d bytes), scratch 0x%x, firmware fingerprint %016x' % (a.out, len(out), scratch, fingerprint))


if __name__ == '__main__':
    main()
