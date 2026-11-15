/* SPDX-FileCopyrightText: 2026 Piotr Krysik */
/* SPDX-License-Identifier: GPL-3.0-only */
/* DSP program: replays a receiver tuning script, i.e. sends a list of RF-chip events through the firmware's own event
 * queue (rflm_ccs_send_common_rf_event), one after another, each after its delay, waiting for each one to finish.
 *
 *   fn(table, count, area)
 *   table: `count` entries of 16 bytes {event address, available length, delay before sending (timer ticks),
 *          status of the event after it ran (written here)}
 *   area : start of the memory that holds the events; its cache lines are invalidated first, so that the events
 *          written by the AP through /dev/mem are seen (AREA_LINES lines of 32 bytes)
 * Returns a bit mask of the events that completed (bit n = event n). Preserves r16-r23.
 *
 * Time is measured on a free-running 22-bit timer of the modem (STMR_COUNTER), so every difference is taken modulo
 * 2^22 (TICK_MASK). An event is complete when the top 4 bits of its status word (EVENT_STATUS_OFF) are EVENT_DONE.
 *
 * Registers: r16 = current table entry, r17 = entries left, r18 = mask of completed events, r19 = &STMR_COUNTER,
 *            r20 = event address (r20 is also the cache line pointer in the first loop), r21 = delay / line count,
 *            r22 = event index, r23 = start tick of the current wait. */

.equ STMR_COUNTER,      0xEC328018    /* free-running modem timer, 22 bits */
.equ TICK_MASK,         0x3fffff      /* 2^22 - 1 */
.equ AREA_LINES,        130           /* cache lines invalidated at `area` (130 * 32 bytes) */
.equ EVENT_STATUS_OFF,  32            /* status word inside an event */
.equ EVENT_DONE,        4             /* status >> 28 when the event has finished */
.equ EVENT_TIMEOUT,     3000000       /* ticks to wait for an event to finish */
.equ EVENT_GAP,         2000          /* ticks between the end of one event and the next one */

.globl fn
fn:
  { allocframe(#48) }                 /* save the callee-saved registers this program uses */
  { memd(sp+#32) = r17:16 }
  { memd(sp+#24) = r19:18 }
  { memd(sp+#16) = r21:20 }
  { memd(sp+#8) = r23:22 }
  { r16 = r0
    r17 = r1 }
  { r18 = #0
    r22 = #0 }
  { r19 = ##STMR_COUNTER }
  { r20 = and(r2, #-32)               /* invalidate the cache lines of the event area */
    r21 = #AREA_LINES }
Linv:
  { dccleaninva(r20) }
  { r20 = add(r20, #32)
    r21 = add(r21, #-1) }
  { p0 = cmp.eq(r21, #0) }
  { if (!p0) jump Linv }

Lnext:                                /* next table entry */
  { p0 = cmp.eq(r17, #0) }
  { if (p0) jump Ldone }
  { r20 = memw(r16+#0) }              /* event address */
  { r21 = memw(r16+#8) }              /* delay before sending */
  { r23 = memw(r19+#0) }
  { r23 = and(r23, ##TICK_MASK) }
Ldly:                                 /* wait until `delay` ticks have passed */
  { r5 = memw(r19+#0) }
  { r5 = and(r5, ##TICK_MASK) }
  { r5 = sub(r5, r23) }
  { r5 = and(r5, ##TICK_MASK) }
  { p0 = cmp.gtu(r21, r5) }
  { if (p0) jump Ldly }
  { r0 = r20                          /* send the event: (event address, available length) */
    r1 = memw(r16+#4) }
  { call rflm_ccs_send_common_rf_event }
  { r23 = memw(r19+#0) }
  { r23 = and(r23, ##TICK_MASK)
    r7 = ##EVENT_TIMEOUT }
Lwd:                                  /* wait for the event to finish, at most EVENT_TIMEOUT ticks */
  { r5 = memw(r20+#EVENT_STATUS_OFF) }
  { r6 = lsr(r5, #28) }
  { p0 = cmp.eq(r6, #EVENT_DONE) }
  { if (p0) jump Lok }
  { r5 = memw(r19+#0) }
  { r5 = and(r5, ##TICK_MASK) }
  { r5 = sub(r5, r23) }
  { r5 = and(r5, ##TICK_MASK) }
  { p0 = cmp.gtu(r5, r7) }
  { if (!p0) jump Lwd }
  { jump Lstore }                     /* timed out: the event's bit stays 0 */
Lok:
  { r2 = #1 }                         /* completed: set bit `index` in the mask */
  { r2 = asl(r2, r22) }
  { r18 = or(r18, r2) }
Lstore:
  { r23 = memw(r19+#0) }
  { r23 = and(r23, ##TICK_MASK) }
Lfl:                                  /* EVENT_GAP ticks before the next event */
  { r5 = memw(r19+#0) }
  { r5 = and(r5, ##TICK_MASK) }
  { r5 = sub(r5, r23) }
  { r5 = and(r5, ##TICK_MASK) }
  { r6 = ##EVENT_GAP }
  { p0 = cmp.gtu(r5, r6) }
  { if (!p0) jump Lfl }
  { r3 = ##rf_ccs_transaction_done }  /* clear the firmware's "transaction done" flag: a stale 1 would break the */
  { r4 = #0 }                         /* next RF-chip write of the firmware */
  { memb(r3+#0) = r4 }
  { r5 = memw(r20+#EVENT_STATUS_OFF) }   /* store the event's status in the table entry */
  { memw(r16+#12) = r5 }
  { r16 = add(r16, #16)
    r17 = add(r17, #-1) }
  { r22 = add(r22, #1) }
  { jump Lnext }

Ldone:
  { r0 = r18 }
  { r17:16 = memd(sp+#32) }
  { r19:18 = memd(sp+#24) }
  { r21:20 = memd(sp+#16) }
  { r23:22 = memd(sp+#8) }
  { dealloc_return }
