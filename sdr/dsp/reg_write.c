/* SPDX-FileCopyrightText: 2026 Piotr Krysik */
/* SPDX-License-Identifier: GPL-3.0-only */
/* DSP program: write one 32-bit register; fn(addr, value) returns the previous value.
 * Only the registers used to configure the sample stream and the sample rate are accepted; any other
 * address returns 0xDEAD0001 without touching anything. */
typedef unsigned u32;

static inline u32 rd(u32 a)
{
    u32 v;
    __asm__ volatile("%0 = memw(%1+#0)" : "=r"(v) : "r"(a) : "memory");
    return v;
}

static inline void wr(u32 a, u32 v)
{
    __asm__ volatile("memw(%0+#0) = %1" ::"r"(a), "r"(v) : "memory");
}

__attribute__((section(".text.entry"))) u32 fn(u32 a, u32 b, u32 c)
{
    (void)c;
    /* narrowband 0 channel filter coefficients */
    if (a >= 0xEC36803Cu && a <= 0xEC368064u && (a & 3u) == 0) {
        u32 old = rd(a);
        wr(a, b);
        return old;
    }
    /* the other registers allowed:
     *   sample-bridge window and transfer control:   0xEC36D000, 0xEC36D004, 0xEC36D028
     *   narrowband writer 0 configuration:           0xEC3680B4, 0xEC338008
     *   sample-rate plan, clock mux and ADC logic:   0xEC181138, 0xEC180068, 0xEC360004
     *   sample-rate plan, decimator and resampler:   0xEC3640B4, 0xEC364088, 0xEC36408C, 0xEC364090 */
    if (a != 0xEC36D000u && a != 0xEC36D004u && a != 0xEC36D028u && a != 0xEC3680B4u && a != 0xEC338008u &&
        a != 0xEC181138u && a != 0xEC180068u && a != 0xEC360004u && a != 0xEC3640B4u && a != 0xEC364088u &&
        a != 0xEC36408Cu && a != 0xEC364090u)
        return 0xDEAD0001u;
    u32 old = rd(a);
    wr(a, b);
    return old;
}
