// SPDX-FileCopyrightText: 2026 Piotr Krysik
// SPDX-License-Identifier: GPL-3.0-only
// pcfl_decode.h - decoding of the packed samples of a record into float I/Q, with an optional digital frequency shift.
//
// A record holds 1280 samples of 20 bits: bits 0-7 Q mantissa, bits 8-15 I mantissa (both signed), bits 16-19 the shared
// exponent e. The float value is mantissa * 2^(1-e) (full scale = 1.0). Two samples take five bytes, so a sample at an
// even index starts on a byte boundary.
//
// The scalar path decodes one sample at a time; on aarch64 sixteen samples (40 bytes) are decoded per step with NEON
// table lookups. Both give identical values without the shift; with the shift they agree to rounding.
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define PCFL_NEON 1
#endif

namespace pcfl {

constexpr size_t PAYLOAD_BYTES = 3200; // 1280 samples x 20 bits

// Digital shift by a phasor recurrence: the current phasor is multiplied by the step for every sample.
struct Nco {
    bool     on = false;
    float    re = 1.0f, im = 0.0f;                                 // current phasor
    float    step_re = 1.0f, step_im = 0.0f;                       // step
    float    pw_re[4] = { 1, 1, 1, 1 }, pw_im[4] = { 0, 0, 0, 0 }; // step^0 .. step^3
    float    s4_re = 1.0f, s4_im = 0.0f;                           // step^4
    uint32_t n = 0;                                                // samples since the last renormalisation point

    // The shift is angle radians per sample.
    void set(double angle, bool enabled)
    {
        on      = enabled;
        re      = 1.0f;
        im      = 0.0f;
        n       = 0;
        step_re = (float)cos(angle);
        step_im = (float)sin(angle);
        for (int k = 0; k < 4; k++) {
            pw_re[k] = (float)cos(angle * k);
            pw_im[k] = (float)sin(angle * k);
        }
        s4_re = (float)cos(angle * 4);
        s4_im = (float)sin(angle * 4);
    }
    void renormalise()
    {
        const float m = 1.0f / sqrtf(re * re + im * im);
        re *= m;
        im *= m;
    }
};

// One sample (mantissas scaled by 2^(1-e)).
inline void get_one(const uint8_t* p, size_t k, float& fi, float& fq)
{
    const size_t bit = k * 20, off = bit >> 3;
    uint32_t     w = 0;
    memcpy(&w,
           p + off,
           off + 4 <= PAYLOAD_BYTES ? 4 : PAYLOAD_BYTES - off); // the last sample must not read past the payload
    const uint32_t v = (w >> (bit & 7)) & 0xFFFFF;
    const int      q = (int8_t)(v & 0xFF), i = (int8_t)((v >> 8) & 0xFF);
    const uint32_t sb = (uint32_t)(128 - (int)((v >> 16) & 0xF)) << 23; // the float 2^(1-e)
    float          scale;
    memcpy(&scale, &sb, 4);
    fi = (float)i * scale;
    fq = (float)q * scale;
}

inline void decode_scalar(const uint8_t* p, size_t k, size_t n, float* out, Nco* nco)
{
    for (size_t j = 0; j < n; j++) {
        float fi, fq;
        get_one(p, k + j, fi, fq);
        if (nco && nco->on) {
            const float ri = fi * nco->re - fq * nco->im, rq = fi * nco->im + fq * nco->re;
            fi             = ri;
            fq             = rq;
            const float nr = nco->re * nco->step_re - nco->im * nco->step_im;
            const float ni = nco->re * nco->step_im + nco->im * nco->step_re;
            nco->re        = nr;
            nco->im        = ni;
            if ((++nco->n & 1023u) == 0)
                nco->renormalise();
        }
        out[2 * j]     = fi;
        out[2 * j + 1] = fq;
    }
}

#ifdef PCFL_NEON
// Sixteen samples starting at an even index; p points at their first byte; 48 bytes are read.
inline void decode16(const uint8_t* p, float* out, Nco* nco)
{
    // byte positions (0..39) of the pieces of each sample; 255 = nothing (the lookup gives 0)
    static const uint8_t QA[16] = { 0, 2, 5, 7, 10, 12, 15, 17, 20, 22, 25, 27, 30, 32, 35, 37 };
    static const uint8_t QB[16] = { 255, 3, 255, 8, 255, 13, 255, 18, 255, 23, 255, 28, 255, 33, 255, 38 };
    static const uint8_t IA[16] = { 1, 3, 6, 8, 11, 13, 16, 18, 21, 23, 26, 28, 31, 33, 36, 38 };
    static const uint8_t IB[16] = { 255, 4, 255, 9, 255, 14, 255, 19, 255, 24, 255, 29, 255, 34, 255, 39 };
    static const uint8_t EA[16] = { 2, 4, 7, 9, 12, 14, 17, 19, 22, 24, 27, 29, 32, 34, 37, 39 };
    static const int8_t SA[16] = { 0, -4, 0, -4, 0, -4, 0, -4, 0, -4, 0, -4, 0, -4, 0, -4 }; // odd samples: high nibble
    static const int8_t SB[16] = { 0, 4, 0, 4, 0, 4, 0, 4, 0, 4, 0, 4, 0, 4, 0, 4 }; // odd samples: low nibble up

    const uint8x16x3_t tb = { { vld1q_u8(p), vld1q_u8(p + 16), vld1q_u8(p + 32) } };
    const int8x16_t    sa = vld1q_s8(SA), sb = vld1q_s8(SB);
    const uint8x16_t   q =
        vorrq_u8(vshlq_u8(vqtbl3q_u8(tb, vld1q_u8(QA)), sa), vshlq_u8(vqtbl3q_u8(tb, vld1q_u8(QB)), sb));
    const uint8x16_t i =
        vorrq_u8(vshlq_u8(vqtbl3q_u8(tb, vld1q_u8(IA)), sa), vshlq_u8(vqtbl3q_u8(tb, vld1q_u8(IB)), sb));
    const uint8x16_t e = vandq_u8(vshlq_u8(vqtbl3q_u8(tb, vld1q_u8(EA)), sa), vdupq_n_u8(15));

    // widen to 4 groups of 4 samples
    const int16x8_t  q16[2] = { vmovl_s8(vget_low_s8(vreinterpretq_s8_u8(q))),
                                vmovl_s8(vget_high_s8(vreinterpretq_s8_u8(q))) };
    const int16x8_t  i16[2] = { vmovl_s8(vget_low_s8(vreinterpretq_s8_u8(i))),
                                vmovl_s8(vget_high_s8(vreinterpretq_s8_u8(i))) };
    const uint16x8_t e16[2] = { vmovl_u8(vget_low_u8(e)), vmovl_u8(vget_high_u8(e)) };

    float32x4_t fI[4], fQ[4];
    for (int g = 0; g < 4; g++) {
        const int         h     = g >> 1;
        const bool        hi    = g & 1;
        const int32x4_t   qi    = hi ? vmovl_s16(vget_high_s16(q16[h])) : vmovl_s16(vget_low_s16(q16[h]));
        const int32x4_t   ii    = hi ? vmovl_s16(vget_high_s16(i16[h])) : vmovl_s16(vget_low_s16(i16[h]));
        const uint32x4_t  ee    = hi ? vmovl_u16(vget_high_u16(e16[h])) : vmovl_u16(vget_low_u16(e16[h]));
        const float32x4_t scale = vreinterpretq_f32_u32(vshlq_n_u32(vsubq_u32(vdupq_n_u32(128), ee), 23));
        fI[g]                   = vmulq_f32(vcvtq_f32_s32(ii), scale);
        fQ[g]                   = vmulq_f32(vcvtq_f32_s32(qi), scale);
    }

    if (nco && nco->on) {
        const float32x4_t pw_re = vld1q_f32(nco->pw_re), pw_im = vld1q_f32(nco->pw_im);
        const float32x4_t rr = vdupq_n_f32(nco->re), ri = vdupq_n_f32(nco->im);
        const float32x4_t s4r = vdupq_n_f32(nco->s4_re), s4i = vdupq_n_f32(nco->s4_im);
        float32x4_t       pre = vmlsq_f32(vmulq_f32(rr, pw_re), ri, pw_im); // phasors of samples 0..3
        float32x4_t       pim = vmlaq_f32(vmulq_f32(rr, pw_im), ri, pw_re);
        for (int g = 0; g < 4; g++) {
            float32x4x2_t o;
            o.val[0] = vmlsq_f32(vmulq_f32(fI[g], pre), fQ[g], pim);
            o.val[1] = vmlaq_f32(vmulq_f32(fI[g], pim), fQ[g], pre);
            vst2q_f32(out + 8 * g, o);
            const float32x4_t nre = vmlsq_f32(vmulq_f32(pre, s4r), pim, s4i);
            const float32x4_t nim = vmlaq_f32(vmulq_f32(pre, s4i), pim, s4r);
            pre                   = nre;
            pim                   = nim;
        }
        nco->re = vgetq_lane_f32(pre, 0); // = phasor * step^16
        nco->im = vgetq_lane_f32(pim, 0);
        nco->n += 16;
        if ((nco->n >> 10) != ((nco->n - 16) >> 10))
            nco->renormalise();
    } else {
        for (int g = 0; g < 4; g++) {
            float32x4x2_t o;
            o.val[0] = fI[g];
            o.val[1] = fQ[g];
            vst2q_f32(out + 8 * g, o);
        }
    }
}
#endif

// Decodes samples k0 .. k0+n-1 of a record payload into n float I/Q pairs (interleaved).
inline void decode_cf32(const uint8_t* p, size_t k0, size_t n, float* out, Nco* nco)
{
    size_t       k   = k0;
    const size_t end = k0 + n;
#ifdef PCFL_NEON
    if ((k & 1) && k < end) {
        decode_scalar(p, k, 1, out, nco);
        out += 2;
        k++;
    }
    while (end - k >= 16 && (k * 5) / 2 + 48 <= PAYLOAD_BYTES) {
        decode16(p + (k * 5) / 2, out, nco);
        out += 32;
        k += 16;
    }
#endif
    if (k < end)
        decode_scalar(p, k, end - k, out, nco);
}

} // namespace pcfl
