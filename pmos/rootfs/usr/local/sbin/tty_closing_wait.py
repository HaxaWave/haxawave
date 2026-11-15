#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Piotr Krysik
# SPDX-License-Identifier: GPL-3.0-only
"""tty_closing_wait.py [DEV] [VALUE_cs] - set the closing_wait of a serial port via TIOCSSERIAL (root).

Default: /dev/ttyMSM0 and 65535 (ASYNC_CLOSING_WAIT_NONE: do not wait for the TX buffer to drain on close).
Without this, PID 1 closes the console in tty_wait_until_sent before exec'ing systemd and waits closing_wait
(3000 cs = 30 s) when nothing is attached to the UART, which adds ~31 s of silence to every reboot.
struct serial_struct (arm64, 72 bytes): closing_wait is a u16 at offset 40.
"""

import fcntl, os, struct, sys

dev = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyMSM0'
val = int(sys.argv[2]) if len(sys.argv) > 2 else 65535
fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY)
buf = bytearray(72)
fcntl.ioctl(fd, 0x541E, buf, True)  # TIOCGSERIAL
(old,) = struct.unpack_from('<H', buf, 40)
struct.pack_into('<H', buf, 40, val)
fcntl.ioctl(fd, 0x541F, buf)  # TIOCSSERIAL
chk = bytearray(72)
fcntl.ioctl(fd, 0x541E, chk, True)
(new,) = struct.unpack_from('<H', chk, 40)
print('%s closing_wait: %d -> %d (cs)' % (dev, old, new))
os.close(fd)
