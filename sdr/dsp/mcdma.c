/* SPDX-FileCopyrightText: 2026 Piotr Krysik */
/* SPDX-License-Identifier: GPL-3.0-only */
/* DSP program: copies the sample ring of narrowband writer 0 into records in DDR using the modem's DMA engine.
 *
 * fn(log) -- `log` is the DSP virtual address of the control block (see below); the block's allocation also
 * carries the ring and, at its end, the stack of this program. The program runs until the stop word is set
 * or the requested number of records has been copied.
 *
 * How it works
 *   The receiver's narrowband writer fills a ring of 30720 samples in the sample memory pool. The ring is split into
 *   24 blocks of 1280 samples (3200 bytes). The writer's head register changes about every millisecond; every change
 *   tells how many samples were written since the previous one, and so how many blocks are complete. Each complete
 *   block is copied to the output ring in DDR with a DMA task of the modem's own driver (up to 6 tasks in flight),
 *   and a record header with a counter and timestamps is written in front of its samples.
 *
 * Build-time constants (compiler options, taken from the firmware image):
 *   -DMC_QTAIL=<addr>  tail counter of the driver's task queue
 *   -DMC_ENQ=<addr>    function that queues one DMA task
 *
 * Control block: u32 words at `log`, described below (CB_*, [n] = word index = byte offset / 4).
 * Record (stride = block + 32 bytes): u32 {seq, t_a, t_b, ring block, t_b - t_a, flags (1 discontinuity, 2 late),
 *   sample counter, tick of the head snapshot} followed by the 1280 packed samples of the block. */

typedef unsigned           u32;
typedef unsigned long long u64;

extern u32 qurt_sysclock_get_hw_ticks(void);

/* ---- Control block: byte offsets of the words ------------------------------------------------------------------- */
enum {
    CB_MAGIC           = 0,  /* [0]   in:  'CHS2' */
    CB_OUT_RING        = 4,  /* [1]   in:  DDR address of the output ring (32-byte aligned) */
    CB_OUT_RECORDS     = 8,  /* [2]   in:  records in the output ring */
    CB_RECORDS_TO_COPY = 12, /* [3]   in:  stop after this many records */
    CB_STOP            = 20, /* [5]   in:  non-zero = stop (cleared at the start) */
    CB_HEAD_REG        = 24, /* [6]   in:  address of the writer head register */
    CB_RING_SAMPLES    = 28, /* [7]   in:  samples in the writer ring */
    CB_BLOCK_BYTES     = 32, /* [8]   in:  bytes in a block */
    CB_STATUS          = 40, /* [10]  out: 1 running, 2 done, 0x8000000N error */
    CB_RECORDS_COPIED  = 44, /* [11]  out: records copied */
    CB_START_TICK      = 48, /* [12]  out */
    CB_END_TICK        = 52, /* [13]  out */
    CB_LATE            = 56, /* [14]  out: records copied too late (the block was about to be overwritten) */
    CB_MAX_COPY_TICKS  = 60, /* [15]  out: longest copy of one block */
    CB_SLIPS           = 64, /* [16]  out: head jumps that do not match the sample rate */
    CB_LAST_HEAD       = 68, /* [17]  out */
    CB_SNAPSHOTS       = 76, /* [19]  out: head changes seen */
    CB_RATE_MUL        = 84, /* [21]  in:  Fs/19.2 MHz in Q30, 0 = 7.68 MHz */
    CB_LAST_INC        = 88, /* [22]  out: samples written between the last two head snapshots */
    CB_LAST_EXPECTED   = 92, /* [23]  out: samples expected in that time */
    CB_RATE_MUL_LIVE   = 456 /* [114] in:  rate multiplier changed by the AP while running (0 = unused) */
};

/* ---- Constants ------------------------------------------------------------------------------------------------ */
#define MAGIC_CHS2 0x32534843u /* 'CHS2' */
#define STATUS_RUNNING 1u
#define STATUS_DONE 2u
#define ERR_BAD_MAGIC 0x80000001u
#define ERR_BAD_PARAMS 0x80000002u
#define ERR_DMA_TIMEOUT 0x8000000Eu

#define BLOCK_SAMPLES 1280u /* samples in a block */
#define RING_BLOCKS 24u     /* blocks in the writer ring (24 * 1280 = 30720 samples) */
#define BLOCK_BYTES 3200u   /* a sample takes 2.5 bytes */
#define RECORD_HEADER 32u
#define RECORD_STRIDE (BLOCK_BYTES + RECORD_HEADER)
/* The writer restarts its address at the start of the ring every 983040 samples (1966080 at the higher sample rates),
 * whatever the ring length. A ring length that divides this period keeps that restart invisible: with another length
 * (e.g. 25600) the writer returns to the start of the ring early and the blocks it skips repeat old data. */
#define RING_SAMPLES 30720u

#define RATE_MUL_7_68MHZ 0x1999999Au /* Fs/19.2 MHz in Q30 for Fs = 7.68 MHz */
#define MAX_IN_FLIGHT 6u             /* the driver queue has 8 slots: at most 6 in flight */
#define SLIP_TOLERANCE 200u          /* samples */
#define LATE_MARGIN 64u              /* samples: late = the block is within this many samples of being overwritten */
#define DMA_TIMEOUT_TICKS 384000u    /* 20 ms of the 19.2 MHz clock */
#define STACK_BYTES 3145728u         /* the control block's allocation (3 MiB); the stack is at its end */

/* Writer head registers: 0xEC368000 + 0x1000 * n (n = 0..4), offset 0xD4. */
#define HEAD_REG_MIN 0xEC368000u
#define HEAD_REG_MAX 0xEC36C0D8u
#define HEAD_REG_OFFSET 0xD4u

/* ---- Access to memory the AP also touches -------------------------------------------------------------------- */
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

/* Write back and drop the cache line of `a`: the next read comes from memory (the AP writes the control block without
 * going through our cache). */
#define CLEAN_INVALIDATE(a) __asm__ volatile("dccleaninva(%0)" ::"r"(a) : "memory")

/* The cache line with the multiplier written by the AP while running. */
#define REFRESH_RATE_MUL(cb) CLEAN_INVALIDATE(((cb) + CB_RATE_MUL_LIVE) & ~31u)
/* The cache line with the stop word. */
#define REFRESH_STOP(cb) CLEAN_INVALIDATE(((cb) + CB_STOP) & ~31u)
/* Write the cache line with the results back, so that the AP sees them. */
#define PUBLISH(cb) __asm__ volatile("dccleaninva(%0)\n\tbarrier" ::"r"(((cb) + CB_RECORDS_COPIED) & ~31u) : "memory")

/* Time in 19.2 MHz ticks in `dt` converted to samples with the rate multiplier. */
static inline u32 ticks_to_samples(u32 dt, u32 rate_mul)
{
    return (u32)(((u64)dt * (u64)rate_mul) >> 30);
}

/* ---- The copy loop -------------------------------------------------------------------------------------------- */
static u32 work(u32 cb) __attribute__((noinline, optimize("Os")));
static u32 work(u32 cb)
{
    u32 task[7] __attribute__((aligned(8))); /* one DMA task of the driver (7 words) */

    /* Parameters from the AP. */
    u32 out_ring = rd(cb + CB_OUT_RING), out_records = rd(cb + CB_OUT_RECORDS),
        records_to_copy = rd(cb + CB_RECORDS_TO_COPY);
    u32 head_reg = rd(cb + CB_HEAD_REG), ring_samples = rd(cb + CB_RING_SAMPLES), block_bytes = rd(cb + CB_BLOCK_BYTES);
    if (rd(cb + CB_MAGIC) != MAGIC_CHS2)
        return ERR_BAD_MAGIC;
    if (head_reg < HEAD_REG_MIN || head_reg > HEAD_REG_MAX || (head_reg & 3) ||
        (head_reg & 0xFFFu) != HEAD_REG_OFFSET || block_bytes != BLOCK_BYTES || ring_samples != RING_SAMPLES ||
        out_records < 8u || records_to_copy == 0 || records_to_copy > 0x7fffffffu || out_ring < 0x80000000u ||
        (out_ring & 31) || out_ring + out_records * RECORD_STRIDE > 0xC0000000u) {
        wr(cb + CB_STATUS, ERR_BAD_PARAMS);
        return ERR_BAD_PARAMS;
    }
    u32 rate_mul = rd(cb + CB_RATE_MUL);
    if (rate_mul == 0u)
        rate_mul = RATE_MUL_7_68MHZ;

    u32 spin    = 0; /* idle passes of the loop since the last look at the stop word */
    u32 copied  = 0; /* records copied */
    u32 out_pos = 0; /* index of the next record in the output ring */
    u32 slips = 0, late = 0, max_copy_ticks = 0, snapshots = 0;
    u32 discontinuity = 0; /* the next record follows a head jump */
    u32 pending       = 0; /* complete blocks that are not copied yet */
    u32 rem           = 0; /* samples of the block being written that are already there */
    u32 next_block    = 0; /* the oldest block that is not copied yet (index in the writer ring) */
    u32 samples       = 0; /* samples written since the start */
    u32 last_head = rd(head_reg), last_tick = qurt_sysclock_get_hw_ticks();
    int have_head = 0; /* the first snapshot has been taken */

    wr(cb + CB_START_TICK, last_tick);
    wr(cb + CB_STATUS, STATUS_RUNNING);
    wr(cb + CB_RECORDS_COPIED, 0);
    wr(cb + CB_STOP, 0);
    wr(cb + CB_LATE, 0);
    wr(cb + CB_MAX_COPY_TICKS, 0);
    wr(cb + CB_SLIPS, 0);
    wr(cb + CB_SNAPSHOTS, 0);
    PUBLISH(cb);

    while (copied < records_to_copy) {
        u32 head = rd(head_reg);

        if (head != last_head) { /* a new head snapshot (every 1 ms) */
            u32 now     = qurt_sysclock_get_hw_ticks();
            /* samples written since the last snapshot, and the time since then */
            u32 written = (head >= last_head) ? head - last_head : head + ring_samples - last_head;
            u32 dt      = now - last_tick;

            REFRESH_RATE_MUL(cb);
            /* the AP may change the rate multiplier while running */
            {
                u32 m = rd(cb + CB_RATE_MUL_LIVE);
                if (m)
                    rate_mul = m;
            }
            /* samples expected in this interval */
            u32 expected = ticks_to_samples(dt, rate_mul);

            if (have_head) {
                samples += written;
                wr(cb + CB_LAST_INC, written);
                wr(cb + CB_LAST_EXPECTED, expected);
                u32 diff = written > expected ? written - expected : expected - written;
                /* the head moved by something else than the rate predicts */
                if (diff > SLIP_TOLERANCE && ring_samples - diff > SLIP_TOLERANCE) {
                    slips++;
                    discontinuity = 1;
                    wr(cb + CB_SLIPS, slips);
                }
                rem += written;
                while (rem >= BLOCK_SAMPLES) {
                    rem -= BLOCK_SAMPLES;
                    pending++;
                }
            } else { /* first snapshot: the block being written is next_block */
                rem        = head;
                next_block = 0;
                while (rem >= BLOCK_SAMPLES) {
                    rem -= BLOCK_SAMPLES;
                    next_block++;
                }
                have_head = 1;
                samples   = head;
            }
            last_head = head;
            last_tick = now;
            snapshots++;
            wr(cb + CB_LAST_HEAD, head);
            wr(cb + CB_SNAPSHOTS, snapshots);

            REFRESH_STOP(cb);
            if (rd(cb + CB_STOP))
                break;

            /* Copy the complete blocks in batches. */
            while (pending && copied < records_to_copy) {
                u32 batch = pending, j, tail0 = rd(MC_QTAIL), t_issued[MAX_IN_FLIGHT], t_done[MAX_IN_FLIGHT],
                    timed_out = 0;
                if (batch > MAX_IN_FLIGHT)
                    batch = MAX_IN_FLIGHT;
                if (batch > records_to_copy - copied)
                    batch = records_to_copy - copied;

                /* The DMA task: words 0 and 1 are zero, word 2 is 0xc38, word 4 is 0x200 and word 6 is the block size;
                 * word 3 is the source and word 5 the destination. */
                task[0] = 0;
                task[1] = 0;
                task[2] = 0xc38u;
                task[4] = 0x200u;
                task[6] = BLOCK_BYTES;
                for (j = 0; j < batch; j++) {
                    u32 block = next_block + j, slot = out_pos + j;
                    if (block >= RING_BLOCKS)
                        block -= RING_BLOCKS;
                    if (slot >= out_records)
                        slot -= out_records;
                    /* source: pool line = 128 B, a block = 25 lines; destination: after the record header */
                    task[3] = ((0x800u + block * 25u) << 16) | 0xffe8u;
                    task[5] = out_ring + slot * RECORD_STRIDE + RECORD_HEADER;
                    __asm__ volatile("barrier" ::: "memory");
                    t_issued[j] = qurt_sysclock_get_hw_ticks();
                    /* indirect call: the target is out of direct range */
                    {
                        void (*volatile enqueue)(void*) = (void (*)(void*))MC_ENQ;
                        enqueue(task);
                    }
                }

                /* Wait until the driver has taken all the tasks of the batch (its queue tail has moved by j + 1). */
                for (j = 0; j < batch && !timed_out; j++) {
                    u32 t_wait = qurt_sysclock_get_hw_ticks();
                    while (((rd(MC_QTAIL) - tail0) & 7u) <= j) {
                        if (qurt_sysclock_get_hw_ticks() - t_wait > DMA_TIMEOUT_TICKS) {
                            timed_out = 1;
                            break;
                        }
                    }
                    t_done[j] = qurt_sysclock_get_hw_ticks();
                }
                if (timed_out) { /* a task did not come back within 20 ms */
                    wr(cb + CB_STATUS, ERR_DMA_TIMEOUT);
                    PUBLISH(cb);
                    return copied;
                }

                /* Write the record headers. */
                for (j = 0; j < batch; j++) {
                    u32 rec = out_ring + out_pos * RECORD_STRIDE;
                    u32 t_a = j ? t_done[j - 1] : t_issued[0], t_b = t_done[j], flags = discontinuity;
                    /* samples written since the start of this block */
                    u32 age = pending * BLOCK_SAMPLES + rem + ticks_to_samples(t_a - last_tick, rate_mul);
                    if (age > ring_samples - LATE_MARGIN) {
                        flags |= 2;
                        late++;
                        wr(cb + CB_LATE, late);
                    }
                    {
                        u32 l = t_b - t_a;
                        if (l > max_copy_ticks) {
                            max_copy_ticks = l;
                            wr(cb + CB_MAX_COPY_TICKS, max_copy_ticks);
                        }
                    }
                    wr(rec + 4, t_a);
                    wr(rec + 8, t_b);
                    wr(rec + 12, next_block);
                    wr(rec + 16, t_b - t_a);
                    wr(rec + 20, flags);
                    wr(rec + 24, samples);
                    wr(rec + 28, last_tick);
                    wr(rec, copied); /* the sequence number is written last */
                    CLEAN_INVALIDATE(rec);
                    __asm__ volatile("barrier" ::: "memory");
                    copied++;
                    wr(cb + CB_RECORDS_COPIED, copied);
                    PUBLISH(cb);
                    out_pos++;
                    if (out_pos == out_records)
                        out_pos = 0;
                    next_block++;
                    if (next_block == RING_BLOCKS)
                        next_block = 0;
                    pending--;
                }
            }
            discontinuity = 0;
        } else if (((++spin) & 0xfffu) == 0) { /* nothing new: now and then look at the stop word */
            REFRESH_STOP(cb);
            if (rd(cb + CB_STOP))
                break;
        }
    }
    wr(cb + CB_END_TICK, qurt_sysclock_get_hw_ticks());
    wr(cb + CB_STATUS, STATUS_DONE);
    PUBLISH(cb);
    return copied;
}

/* Entry: switch to a stack at the end of the control block's 3 MiB allocation (the sandbox stack is only 4 KB),
 * then run. */
__attribute__((section(".text.entry"), optimize("Os"))) u32 fn(u32 a, u32 b, u32 c)
{
    register u32 r0v __asm__("r0") = a;
    register u32 r1v __asm__("r1") = b;
    register u32 r2v __asm__("r2") = c;
    register u32 r3v __asm__("r3") = a + STACK_BYTES - 64u;
    register u32 r4v __asm__("r4") = (u32)work;
    /* clang-format off */
    __asm__ volatile("{ r16 = r29 }\n\t{ r29 = r3 }\n\t{ callr r4 }\n\t{ r29 = r16 }"
                     : "+r"(r0v)
                     : "r"(r1v), "r"(r2v), "r"(r3v), "r"(r4v)
                     : "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "r16", "r28", "r31",
                       "p0", "p1", "p2", "p3", "memory");
    /* clang-format on */
    return r0v;
}
