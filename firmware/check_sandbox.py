#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Piotr Krysik
# SPDX-License-Identifier: GPL-3.0-only
"""check_sandbox.py [ping] - run ON THE PHONE as root: check that the custom modem firmware (rf_sandbox task) is
running.

Reads the address table that the task publishes in IMEM (0x08600F00, magic "SBFR"), prints the control block and the
task counter, and with `ping` sends the no-operation command (0) and waits for the acknowledgement. The task counter
increases once per second while the firmware runs; the command is acknowledged within about one second.

Modem DDR is read and written with pread()/pwrite() on /dev/mem. Do not use slices of an mmap() of /dev/mem for this
memory: it is device memory and a wide or unaligned access (which Python's slicing may do) ends in a bus error.
"""

import os, struct, sys, time

MODEM_VA_BASE = 0xC0000000  # virtual address the modem firmware is linked at
MODEM_PA_BASE = 0x8E800000  # physical address of the modem region (see the device tree)
IMEM_TABLE = 0x08600F00
MAGIC_TABLE = 0x52464253

fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
rd = lambda pa, n: os.pread(fd, n, pa)
wr = lambda pa, d: os.pwrite(fd, d, pa)
pa_of = lambda va: va - MODEM_VA_BASE + MODEM_PA_BASE

t = struct.unpack('<6I', rd(IMEM_TABLE, 24))
if t[0] != MAGIC_TABLE:
    sys.exit('no sandbox table in IMEM: the custom firmware is not running (sudo qcom-sdr-switch-fw custom)')
ctrl_va, scratch_va, counter_va, started_va = t[2:6]
print(
    'table version %d: ctrl %#x, scratch %#x, counter %#x, started %#x'
    % (t[1], ctrl_va, scratch_va, counter_va, started_va)
)
c = struct.unpack('<10I', rd(pa_of(ctrl_va), 40))
print('control block: magic=%#x seq=%d ack=%d cmd=%d status=%d period=%d ms' % (c[0], c[1], c[2], c[3], c[6], c[9]))
cnt = lambda: struct.unpack('<I', rd(pa_of(counter_va), 4))[0]
print('task started=%d, counter=%d' % (struct.unpack('<I', rd(pa_of(started_va), 4))[0], cnt()))
if len(sys.argv) > 1 and sys.argv[1] == 'ping':
    seq = (c[1] + 1) & 0xFFFFFFFF
    wr(pa_of(ctrl_va) + 12, struct.pack('<III', 0, 0, 0))  # cmd = 0 (no operation), arg0, arg1
    wr(pa_of(ctrl_va) + 4, struct.pack('<I', seq))
    t0 = time.time()
    while time.time() - t0 < 12:
        c = struct.unpack('<10I', rd(pa_of(ctrl_va), 40))
        if c[2] == seq:
            print(
                'acknowledged after %.2f s: status=%d%s'
                % (time.time() - t0, c[6], ' (OK)' if c[6] == 1 else ' (UNEXPECTED)')
            )
            break
        time.sleep(0.05)
    else:
        sys.exit('no acknowledgement within 12 s')
