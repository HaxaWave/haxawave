#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Piotr Krysik
# SPDX-License-Identifier: GPL-3.0-only
"""check_devmem.py [--modem] - run ON THE PHONE as root: check that /dev/mem works with the patched kernel.

Prints the kernel version, the DEVMEM/STRICT_DEVMEM settings from /proc/config.gz and the size of the
reserved mpss region from the device tree (expected 0x5800000 = 88 MiB). Then reads each test address
through both mmap(/dev/mem) and pread(/dev/mem); both must work.

Default addresses (read-only): IMEM 0x08600000 (outside RAM) and SMEM 0x86300000 (reserved, no-map).
--modem  also read the start of the modem region (0x8e800000). Use it only while the modem firmware is running
         and its XPU allows access: reading a protected area may end in an SError or a reboot.
Exit status 0 = everything passed.
"""

import glob, gzip, mmap, os, struct, sys


def read_file(path):
    try:
        return open(path, 'rb').read()
    except OSError:
        return None


print('kernel:', os.uname().release, os.uname().version.split()[0])
cfg = read_file('/proc/config.gz')
if cfg:
    text = gzip.decompress(cfg).decode()
    for key in ('CONFIG_DEVMEM', 'CONFIG_STRICT_DEVMEM', 'CONFIG_IO_STRICT_DEVMEM'):
        line = [l for l in text.splitlines() if l.startswith(key + '=') or l == '# %s is not set' % key]
        print('  ', line[0] if line else '%s: not present' % key)
else:
    print('   /proc/config.gz not available')
for d in glob.glob('/sys/firmware/devicetree/base/reserved-memory/mpss@*'):
    reg = read_file(d + '/reg')
    if reg:
        ah, al, sh, sl = struct.unpack('>4I', reg[:16])
        size = (sh << 32) | sl
        print('   mpss: address %#x size %#x (%.0f MiB)' % ((ah << 32) | al, size, size / 2**20))

targets = [('IMEM 0x08600000', 0x08600000), ('SMEM 0x86300000', 0x86300000)]
if '--modem' in sys.argv:
    targets.append(('MPSS 0x8e800000', 0x8E800000))
ok = True
try:
    fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
except OSError as e:
    sys.exit('cannot open /dev/mem (%s): run as root on the phone; the kernel needs CONFIG_DEVMEM=y' % e)
for name, pa in targets:
    res = []
    for how in ('mmap', 'pread'):
        try:
            if how == 'mmap':
                base = pa & ~0xFFF
                m = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ, offset=base)
                data = m[(pa - base) : (pa - base) + 16]
                m.close()
            else:
                data = os.pread(fd, 16, pa)
            res.append('%s=OK %s' % (how, data.hex()))
        except Exception as e:
            ok = False
            res.append('%s=FAILED %s' % (how, e))
    print('%-18s %s' % (name, '  |  '.join(res)))
print('RESULT:', 'OK' if ok else 'FAILURES (see above)')
sys.exit(0 if ok else 1)
