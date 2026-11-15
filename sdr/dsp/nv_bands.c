/* SPDX-FileCopyrightText: 2026 Piotr Krysik */
/* SPDX-License-Identifier: GPL-3.0-only */
/* DSP program: calls the firmware's per-band table builder with a band mask.
 * fn(addr) -- addr is the DSP virtual address of {u64 band mask (bit n-1 = band n); u64 extension}.
 * Returns the builder's result. */
typedef unsigned u32;

extern u32 rflte_nv_set_card_defaults(void* mask);

__attribute__((section(".text.entry"))) u32 fn(u32 a, u32 b, u32 c)
{
    (void)b;
    (void)c;
    return rflte_nv_set_card_defaults((void*)a);
}
