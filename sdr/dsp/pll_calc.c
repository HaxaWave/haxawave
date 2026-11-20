/* SPDX-FileCopyrightText: 2026 Piotr Krysik */
/* SPDX-License-Identifier: GPL-3.0-only */
/* DSP program: the oscillator words of the receiver chip, computed by the firmware's own routine
 *   wtr4905_common_instance_class::calc_pll_programming(...)
 * that the chip driver calls with the receive frequency in Hz when it tunes.
 *
 * fn(freq_hz, mult | sel << 31) returns the first four (sel 0) or the last two (sel 1) bytes of the six-byte result:
 * [fractional part, 24 bits big-endian] [integer field, 2 bytes] [flag].
 *
 * The parameters are those of the driver's own call: the reference is 38.4 MHz (t = 384 in units of 100 kHz), mult is the
 * ratio between the oscillator and the receive frequency (1, or 3 for the band 20 script, whose oscillator runs at three
 * times the receive frequency), and the rest is no frequency halving, no thresholds, divider 1. They are specific to the
 * WTR4905 driver: another transceiver has another routine, with parameters to take from the call that its driver makes.
 * Only the routine is called; nothing in the firmware image is patched. */
typedef unsigned long long u64;
typedef unsigned short     u16;
typedef unsigned char      u8;
typedef unsigned           u32;

extern void calc_pll(void* self, u32 lut, u64 f, u16 t, u8 h, int a, int b, int c, int d, int e, u8* out) __asm__(
    "_ZN29wtr4905_common_instance_class20calc_pll_programmingEyth23wtr4905_pll_config_typeh31wtr4905_pll_dsmsign_"
    "thrsld_typehP35wtr4905_pll_programming_values_type");

__attribute__((section(".text.entry"))) u32 fn(u32 f, u32 b, u32 c)
{
    u8 out[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    (void)c;
    calc_pll((void*)0, 0, (u64)f, 384, (u8)(b & 0xff), 1, 0, 0, 0, 1, out);
    if (b >> 31)
        return (u32)out[4] | ((u32)out[5] << 8);
    return (u32)out[0] | ((u32)out[1] << 8) | ((u32)out[2] << 16) | ((u32)out[3] << 24);
}
