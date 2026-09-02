/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sim_clock.h - the ONE virtual clock every simulated node's `ops.now_ms` and
 * `ops.now_vms` reads, and the timer wheel its `ops.arm_timer` writes into
 * (FC-P1.4).
 *
 * DESIGN §3.9 RULE 6, TAKEN LITERALLY. "Deadlines and identities are injected
 * (ops.now), never read from the substrate inside an FSM -- so a test drives
 * time." The rung-2 simulator is the largest consumer of that rule: a
 * three-node cluster forming virtual circuits under 10 % loss involves a 2 s
 * HELLO cadence, a 2 s formation retry and a 16 s TIMVCFAIL, and the whole
 * scenario has to run in microseconds and produce the same answer every time.
 * So there is no wall clock anywhere in this harness. Time advances only
 * because the engine moved it to the next scheduled event.
 *
 * IT IS A WHEEL, NOT A POLL. The engine does not tick the FSMs on a fixed
 * interval and hope. It honours the timers the FSM actually armed, exactly as
 * the production fork module does with cf_timer_arm: each (node, timer
 * identity, key) has at most one deadline, re-arming MOVES it (`struct pe_ops`
 * arm_timer contract: "Re-arming an armed timer moves it; it never stacks"),
 * and cancelling removes it. That means an armed-but-never-fired timer is a
 * bug the harness can SEE, instead of a stall the poll would paper over.
 *
 * TWO CLOCKS, BECAUSE THE WIRE NEEDS TWO. vms_pe.h §2 is explicit that now_ms
 * and now_vms are separate primitives and are not convertible: now_ms is a
 * monotonic tick with an arbitrary origin, now_vms is a REAL VMS absolute-time
 * quadword (100 ns units since 17-NOV-1858) that the START/STACK body carries.
 * This clock therefore holds an ORIGIN for the absolute-time half and derives
 * the rest from the elapsed millisecond count. The origin is the SIMULATOR's
 * own clock origin, declared in one place with its value visible: it stands
 * for "when this simulated cluster was powered on" and it is never a quadword
 * lifted out of a capture (the replayed-incarnation bug spec §4(g) records).
 */
#ifndef OVMX_SIM_CLOCK_H
#define OVMX_SIM_CLOCK_H

#include <stdint.h>

/*
 * Enough for 8 nodes x (1 HELLO beat + a channel timer, a retransmit timer and
 * a TIMVCFAIL timer per peer). Overflow is COUNTED and every scenario asserts
 * it is zero -- a silently dropped timer would turn a real stall into a green
 * test, which is the one failure mode this harness must not have.
 */
#define SIM_MAX_TIMERS 512

struct sim_timer {
	uint8_t  in_use;
	uint8_t  node;      /* which simulated node armed it */
	uint8_t  which;     /* enum pe_timer, kept as a byte to stay pure */
	uint8_t  pad;
	uint32_t key;       /* the FSM's per-object key (channel / circuit index) */
	uint64_t due_ms;
	uint64_t serial;    /* arm order, the last tie-break in the ordering */
};

struct sim_clock {
	uint64_t now_ms;
	uint64_t vms_origin;    /* absolute-time origin, 100 ns units */
	uint64_t serial;
	uint32_t n_slots;       /* high-water mark of the table */
	uint32_t arms, moves, cancels, fires, overflows;
	struct sim_timer t[SIM_MAX_TIMERS];
};

/*
 * The simulator's absolute-time origin. A VMS quadword (100 ns units since
 * 17-NOV-1858) that stands for the instant a simulated cluster powers on. It
 * is the harness's OWN number, chosen once and printed in every dump, so a
 * reader can always tell a simulated incarnation from a real one.
 */
#define SIM_VMS_ORIGIN 0x00c0000000000000ull

void     sim_clock_init(struct sim_clock *c, uint64_t vms_origin);

/* What a node's ops.now_ms / ops.now_vms return. now_ms truncates to the 32
 * bits the seam's contract defines; every deadline comparison inside the FSM
 * is wrap-safe, so the truncation is the production behaviour, not a fudge. */
uint32_t sim_clock_now_ms(const struct sim_clock *c);
uint64_t sim_clock_now_vms(const struct sim_clock *c);

/* Arm (or MOVE) / cancel one timer identity. */
void sim_clock_arm(struct sim_clock *c, uint8_t node, uint8_t which,
		   uint32_t key, uint32_t ms);
void sim_clock_cancel(struct sim_clock *c, uint8_t node, uint8_t which,
		      uint32_t key);

/*
 * The earliest armed timer, by (due_ms, node, which, key, serial) -- a TOTAL
 * order, so two runs on the same seed fire the same timer first even when two
 * deadlines coincide. Returns 1 and fills `*slot`, or 0 when nothing is armed.
 */
int sim_clock_next(const struct sim_clock *c, uint32_t *slot);

/* Take the timer out of the wheel and hand back a copy. One-shot, like the
 * production seam: a handler that wants another deadline arms one. */
int sim_clock_fire(struct sim_clock *c, uint32_t slot, struct sim_timer *out);

/* How many timers are armed right now (a scenario asserts on this: a circuit
 * with no armed retransmit timer is a circuit that will never retry). */
uint32_t sim_clock_armed(const struct sim_clock *c);

#endif /* OVMX_SIM_CLOCK_H */
