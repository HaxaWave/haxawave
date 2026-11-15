#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Piotr Krysik
# SPDX-License-Identifier: GPL-3.0-only
"""
pil_split.py - split a finished modem image (qdsp6sw.mbn from the modem_proc build) into the files the PIL loader
on the phone expects: modem.mdt + modem.bNN.

Nothing is recomputed except the relocatable flag (see below). Segment contents and the hash table come
unchanged from the image produced by the official build (sectools), so the MBN header (signature_ptr,
cert_chain_ptr, sizes) stays consistent. Building those structures by hand is rejected by the modem boot
authenticator ("-3 for headers").

Layout:
  modem.b00 = ELF header + program headers          (PH0, PT_NULL, offset 0)
  modem.b01 = MBN header + hash table + sig + cert  (PH1, PT_NULL, hash segment)
  modem.mdt = modem.b00 + modem.b01
  modem.bNN = raw bytes of segment NN, only when p_filesz > 0

Usage:
  pil_split.py qdsp6sw.mbn -o outdir [--name modem]
  pil_split.py --verify outdir [--name modem]
"""

import argparse
import hashlib
import os
import struct
import sys

EHDR_SIZE = 52
PHDR_SIZE = 32
HASH_ENTRY_SIZE = 32
MBN_HEADER_SIZE = 40
HASH_SEG_INDEX = 1


def phdrs_of(data):
    if data[:4] != b'\x7fELF':
        sys.exit("not an ELF file")
    (e_phoff,) = struct.unpack_from('<I', data, 28)
    e_phentsize, e_phnum = struct.unpack_from('<HH', data, 42)
    out = []
    for i in range(e_phnum):
        out.append(struct.unpack_from('<8I', data, e_phoff + i * e_phentsize))
    return e_phoff, e_phentsize, e_phnum, out


def split(img_path, outdir, name):
    data = open(img_path, 'rb').read()
    e_phoff, e_phentsize, e_phnum, phs = phdrs_of(data)
    b00_len = EHDR_SIZE + e_phnum * PHDR_SIZE
    if e_phoff != EHDR_SIZE:
        print("WARNING: e_phoff=%d != %d" % (e_phoff, EHDR_SIZE))

    os.makedirs(outdir, exist_ok=True)
    written = []
    for i, (t, off, va, pa, fsz, msz, fl, al) in enumerate(phs):
        if i == 0:
            blob = data[:b00_len]
            if fsz and fsz != b00_len:
                print("WARNING: PH0 filesz=%d != headers=%d" % (fsz, b00_len))
        else:
            blob = data[off : off + fsz]
            if len(blob) != fsz:
                sys.exit("PH%d: truncated file (%d < %d)" % (i, len(blob), fsz))
        if i > 1 and fsz == 0:
            continue  # no file for empty segments, same as the stock image
        p = os.path.join(outdir, '%s.b%02d' % (name, i))
        open(p, 'wb').write(blob)
        written.append((i, len(blob)))

    # Set QCOM_MDT_RELOCATABLE (0x08000000) in the flags of every PT_LOAD segment. The mainline kernel
    # (qcom_q6v5_mss.c) then uses the lowest segment address as the relocation base instead of the fixed
    # mpss address; without the flag a non-relocatable image fails with "segment outside memory range".
    # After changing b00, update its SHA256 in the hash table (b01 at MBN header + 0).
    QCOM_MDT_RELOCATABLE = 0x08000000
    b00 = bytearray(open(os.path.join(outdir, '%s.b00' % name), 'rb').read())
    b01 = bytearray(open(os.path.join(outdir, '%s.b01' % name), 'rb').read())

    reloc_count = 0
    (b00_phoff,) = struct.unpack_from('<I', b00, 28)
    b00_phnum = struct.unpack_from('<H', b00, 44)[0]
    for i in range(b00_phnum):
        off = b00_phoff + i * PHDR_SIZE
        (p_type,) = struct.unpack_from('<I', b00, off)
        (p_flags,) = struct.unpack_from('<I', b00, off + 24)
        if p_type == 1:  # PT_LOAD
            new_flags = p_flags | QCOM_MDT_RELOCATABLE
            if new_flags != p_flags:
                struct.pack_into('<I', b00, off + 24, new_flags)
                reloc_count += 1
    if reloc_count:
        # update the b00 hash in the table (b01[MBN_HEADER_SIZE + 0*32 : +32])
        new_hash = hashlib.sha256(bytes(b00)).digest()
        b01[MBN_HEADER_SIZE : MBN_HEADER_SIZE + HASH_ENTRY_SIZE] = new_hash
        open(os.path.join(outdir, '%s.b00' % name), 'wb').write(b00)
        open(os.path.join(outdir, '%s.b01' % name), 'wb').write(b01)
        print("relocatable: %d PT_LOAD segments updated, b00 hash recomputed" % reloc_count)

    open(os.path.join(outdir, '%s.mdt' % name), 'wb').write(bytes(b00) + bytes(b01))

    print(
        "image=%s  phnum=%d  entry=0x%08x"
        % (os.path.basename(img_path), e_phnum, struct.unpack_from('<I', data, 24)[0])
    )
    print("mdt=%dB (b00=%dB + b01=%dB), bNN files=%d" % (len(b00) + len(b01), len(b00), len(b01), len(written)))
    return 0


def verify(outdir, name):
    """Check what the modem boot authenticator checks: the sha256 of every modem.bNN against its entry
    in the hash table inside modem.mdt."""
    mdt = open(os.path.join(outdir, '%s.mdt' % name), 'rb').read()
    e_phoff, e_phentsize, e_phnum, phs = phdrs_of(mdt)
    b00_len = EHDR_SIZE + e_phnum * PHDR_SIZE
    base = b00_len + MBN_HEADER_SIZE

    (img_id, hdr_vsn, _r, dest, total, tbl_len, sig_ptr, sig_sz, cert_ptr, cert_sz) = struct.unpack_from(
        '<10I', mdt, b00_len
    )
    print(
        "MBN: image_id=%d vsn=%d dest=0x%08x total=%d table=%d "
        "sig=0x%08x/%d cert=0x%08x/%d" % (img_id, hdr_vsn, dest, total, tbl_len, sig_ptr, sig_sz, cert_ptr, cert_sz)
    )
    if sig_sz == 0 or cert_sz == 0:
        print("  FAIL sig_sz/cert_sz == 0 -> the boot authenticator will reject it ('-3 for headers')")

    if tbl_len != e_phnum * HASH_ENTRY_SIZE:
        print("  WARNING table_len=%d != phnum*32=%d" % (tbl_len, e_phnum * 32))

    ok = zero = bad = 0
    for i, (t, off, va, pa, fsz, msz, fl, al) in enumerate(phs):
        want = mdt[base + i * HASH_ENTRY_SIZE : base + (i + 1) * HASH_ENTRY_SIZE]
        path = os.path.join(outdir, '%s.b%02d' % (name, i))
        if not os.path.exists(path):
            if any(want):
                print("  FAIL b%02d: file missing but hash is non-zero" % i)
                bad += 1
            else:
                zero += 1
            continue
        blob = open(path, 'rb').read()
        if not any(want):
            zero += 1
            continue
        if hashlib.sha256(blob).digest() == want:
            ok += 1
        else:
            print("  FAIL b%02d: sha256 != hash-table (len=%d, filesz=%d)" % (i, len(blob), fsz))
            bad += 1
    print("verify: phnum=%d  hash_ok=%d  zero=%d  errors=%d" % (e_phnum, ok, zero, bad))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image', nargs='?')
    ap.add_argument('-o', '--outdir', default='pil_out')
    ap.add_argument('--name', default='modem')
    ap.add_argument('--verify', metavar='DIR')
    a = ap.parse_args()
    if a.verify:
        sys.exit(verify(a.verify, a.name))
    if not a.image:
        ap.error("give an image or --verify DIR")
    split(a.image, a.outdir, a.name)
    sys.exit(verify(a.outdir, a.name))


if __name__ == '__main__':
    main()
