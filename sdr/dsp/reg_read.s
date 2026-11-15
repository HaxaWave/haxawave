/* SPDX-FileCopyrightText: 2026 Piotr Krysik */
/* SPDX-License-Identifier: GPL-3.0-only */
/* DSP program: r0 = *(u32 *)r0  (reads one 32-bit word from a DSP virtual address) */
.globl fn
fn:
  { r0 = memw(r0+#0) }
  { jumpr r31 }
