/* SPDX-FileCopyrightText: 2026 Piotr Krysik */
/* SPDX-License-Identifier: GPL-3.0-only */
/* DSP program: r0 = modem_mem_alloc(r0 bytes, 0); returns the DSP virtual address or 0 */
.globl fn
fn:
  { allocframe(#0) }
  { r1 = #0 }
  { call modem_mem_alloc }
  { dealloc_return }
