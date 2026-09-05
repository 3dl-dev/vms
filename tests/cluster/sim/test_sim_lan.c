/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_sim_lan.c - the harness tests ITSELF before it is allowed to be
 * evidence about the stack (FC-P1.4).
 *
 * A simulator whose wire model is untested is worse than no simulator: a
 * scenario that goes green because the virtual LAN silently delivered
 * everything proves nothing, and a scenario that goes red because the reorder
 * model is broken costs a day chasing the FSM. So every mechanism the rung-2
 * harness rests on has a test here, driven with a stub receiver rather than
 * with the FSM:
 *
 *   the RNG        same seed same sequence; different seed different; a
 *                  disabled condition consumes no draw; the streams are
 *                  independent per link
 *   the clock      arming MOVES a deadline and never stacks it; cancel
 *                  removes; the firing order is total
 *   the LAN        loss, duplication, reordering, latency, asymmetry, a cut,
 *                  a down port, unicast vs multicast group membership
 */

#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "sim_clock.h"
#include "sim_lan.h"
#include "sim_rng.h"

/* ------------------------------------------------------------------ *
 * 1. The RNG
 * ------------------------------------------------------------------ */

static void test_rng_determinism(void)
{
	struct sim_rng a, b, c;
	int i, same = 1, differs = 0;

	printf("-- the RNG: same seed same stream, different seed different\n");
	sim_rng_seed(&a, 1234u, 7u);
	sim_rng_seed(&b, 1234u, 7u);
	sim_rng_seed(&c, 1235u, 7u);
	for (i = 0; i < 64; i++) {
		uint64_t va = sim_rng_u64(&a);

		if (va != sim_rng_u64(&b))
			same = 0;
		if (va != sim_rng_u64(&c))
			differs = 1;
	}
	ct_check(same, "the same (seed, stream) replays exactly");
	ct_check(differs, "a different seed does not");
}

static void test_rng_streams_are_independent(void)
{
	struct sim_rng a, b;
	int i, differs = 0;

	printf("-- two links on one seed do not share a sequence\n");
	sim_rng_seed(&a, 99u, 1u);
	sim_rng_seed(&b, 99u, 2u);
	for (i = 0; i < 32; i++) {
		if (sim_rng_u64(&a) != sim_rng_u64(&b))
			differs = 1;
	}
	ct_check(differs, "stream 1 and stream 2 diverge");
}

static void test_rng_pct(void)
{
	struct sim_rng r;
	unsigned i, hits = 0u;
	uint64_t draws_before;

	printf("-- a Bernoulli draw: 0 and 100 are free, 10 %% is about 10 %%\n");
	sim_rng_seed(&r, 5u, 5u);
	draws_before = r.draws;
	ct_check(!sim_rng_pct(&r, 0u), "0 %% never fires");
	ct_check(sim_rng_pct(&r, 100u), "100 %% always fires");
	ct_check_eq_u32((unsigned long)(r.draws - draws_before), 0,
			"and neither consumed a draw (a disabled condition "
			"must not shift the link beside it)");

	for (i = 0; i < 10000u; i++) {
		if (sim_rng_pct(&r, 10u))
			hits++;
	}
	/* 10 % of 10 000 with a 3-sigma band of ~90; 200 is generous and this
	 * is a fixed seed, so it is a constant, not a flake. */
	ct_check(hits > 800u && hits < 1200u,
		 "10 % of 10 000 draws landed in [800, 1200]");
	printf("       (measured %u of 10000)\n", hits);
}

/* ------------------------------------------------------------------ *
 * 2. The clock and the timer wheel
 * ------------------------------------------------------------------ */

/* The pe_timer identities, by value: this file must not include the FSM. */
#define T_HELLO      0u
#define T_CHANNEL    1u
#define T_RETRANSMIT 2u

static void test_clock_arm_moves_never_stacks(void)
{
	struct sim_clock c;
	struct sim_timer t;
	uint32_t slot;

	printf("-- arming an armed timer MOVES it (the pe_ops contract)\n");
	sim_clock_init(&c, SIM_VMS_ORIGIN);
	sim_clock_arm(&c, 0u, T_HELLO, 0u, 2000u);
	sim_clock_arm(&c, 0u, T_HELLO, 0u, 500u);
	ct_check_eq_u32(sim_clock_armed(&c), 1, "still exactly one timer");
	ct_check_eq_u32(c.arms, 1, "one arm");
	ct_check_eq_u32(c.moves, 1, "and one move");

	ct_check(sim_clock_next(&c, &slot), "it is the next timer");
	ct_check(sim_clock_fire(&c, slot, &t), "and it fires");
	ct_check_eq_u32((unsigned long)t.due_ms, 500,
			"at the MOVED deadline, not the original");
	ct_check_eq_u32(sim_clock_armed(&c), 0, "one-shot: the wheel is empty");
}

static void test_clock_cancel(void)
{
	struct sim_clock c;
	uint32_t slot;

	printf("-- cancel removes exactly the named identity\n");
	sim_clock_init(&c, SIM_VMS_ORIGIN);
	sim_clock_arm(&c, 0u, T_CHANNEL, 3u, 100u);
	sim_clock_arm(&c, 0u, T_CHANNEL, 4u, 100u);
	sim_clock_cancel(&c, 0u, T_CHANNEL, 3u);
	ct_check_eq_u32(sim_clock_armed(&c), 1, "one left");
	ct_check(sim_clock_next(&c, &slot), "and it is findable");
	ct_check_eq_u32(c.t[slot].key, 4, "the one that was not cancelled");
}

static void test_clock_order_is_total(void)
{
	struct sim_clock c;
	struct sim_timer t;
	uint32_t slot;

	printf("-- equal deadlines fire in a FIXED order (node, timer, key)\n");
	sim_clock_init(&c, SIM_VMS_ORIGIN);
	/* Armed out of order, all due at the same instant. */
	sim_clock_arm(&c, 2u, T_RETRANSMIT, 1u, 1000u);
	sim_clock_arm(&c, 1u, T_HELLO, 0u, 1000u);
	sim_clock_arm(&c, 1u, T_CHANNEL, 5u, 1000u);
	sim_clock_arm(&c, 1u, T_CHANNEL, 2u, 1000u);

	ct_check(sim_clock_next(&c, &slot) && sim_clock_fire(&c, slot, &t),
		 "first fires");
	ct_check(t.node == 1u && t.which == T_HELLO, "node 1, HELLO");
	ct_check(sim_clock_next(&c, &slot) && sim_clock_fire(&c, slot, &t),
		 "second fires");
	ct_check(t.node == 1u && t.which == T_CHANNEL && t.key == 2u,
		 "node 1, CHANNEL key 2 (lower key first)");
	ct_check(sim_clock_next(&c, &slot) && sim_clock_fire(&c, slot, &t),
		 "third fires");
	ct_check(t.key == 5u, "node 1, CHANNEL key 5");
	ct_check(sim_clock_next(&c, &slot) && sim_clock_fire(&c, slot, &t),
		 "fourth fires");
	ct_check(t.node == 2u, "node 2 last");
	ct_check(!sim_clock_next(&c, &slot), "the wheel is empty");
}

static void test_clock_two_time_bases(void)
{
	struct sim_clock c;

	printf("-- now_ms and now_vms are two clocks, not one scaled\n");
	sim_clock_init(&c, SIM_VMS_ORIGIN);
	ct_check_eq_u32(sim_clock_now_ms(&c), 0, "the tick starts at zero");
	ct_check(sim_clock_now_vms(&c) == SIM_VMS_ORIGIN,
		 "absolute time starts at the declared origin");
	c.now_ms = 1234u;
	ct_check_eq_u32(sim_clock_now_ms(&c), 1234, "the tick tracks");
	ct_check(sim_clock_now_vms(&c) == SIM_VMS_ORIGIN + 12340000ull,
		 "and absolute time advances in 100 ns units");
}

/* ------------------------------------------------------------------ *
 * 3. The virtual LAN
 * ------------------------------------------------------------------ */

static const uint8_t mac_a[6] = { 0x02, 0, 0, 0, 0, 0x0a };
static const uint8_t mac_b[6] = { 0x02, 0, 0, 0, 0, 0x0b };
static const uint8_t mac_c[6] = { 0x02, 0, 0, 0, 0, 0x0c };
static const uint8_t group1[6] = { 0xab, 0x00, 0x04, 0x01, 0x01, 0x01 };
static const uint8_t group2[6] = { 0xab, 0x00, 0x04, 0x01, 0x02, 0x02 };

/* A minimal frame: destination, source, an ethertype and a payload byte the
 * test can tell apart. Nothing here is an SCA frame -- this test is about the
 * WIRE, not about the protocol. */
static uint32_t mkframe(uint8_t *out, const uint8_t dst[6],
			const uint8_t src[6], uint8_t tag)
{
	memset(out, 0, 64);
	memcpy(out, dst, 6);
	memcpy(out + 6, src, 6);
	out[12] = 0x60;
	out[13] = 0x07;
	out[14] = tag;
	return 64u;
}

/* Drain everything scheduled, in order, into a caller's tag list. */
static uint32_t drain(struct sim_lan *lan, uint8_t *tags, uint8_t *tos,
		      uint32_t max)
{
	uint32_t n = 0u, slot;

	while (n < max && sim_lan_next(lan, &slot)) {
		const struct sim_inflight *e = sim_lan_peek(lan, slot);

		tags[n] = e->b[14];
		tos[n] = e->to;
		n++;
		sim_lan_take(lan, slot);
	}
	return n;
}

static void lan_three_nodes(struct sim_lan *lan, uint64_t seed)
{
	sim_lan_init(lan, seed);
	(void)sim_lan_add_port(lan, mac_a, group1);
	(void)sim_lan_add_port(lan, mac_b, group1);
	(void)sim_lan_add_port(lan, mac_c, group1);
}

static void test_lan_perfect_wire(void)
{
	struct sim_lan lan;
	uint8_t f[64], tags[8], tos[8];
	uint32_t n;

	printf("-- a wire with no conditions delivers exactly once\n");
	lan_three_nodes(&lan, 1u);
	(void)mkframe(f, mac_b, mac_a, 0x11);
	(void)sim_lan_xmit(&lan, 0u, f, 64u, 0u);
	n = drain(&lan, tags, tos, 8u);
	ct_check_eq_u32(n, 1, "one copy");
	ct_check_eq_u32(tos[0], 1, "to the port that owns that address");
	ct_check_eq_u32((unsigned long)lan.lost, 0, "nothing lost");
}

static void test_lan_loss_and_asymmetry(void)
{
	struct sim_lan lan;
	struct sim_link off, dead;
	uint8_t f[64], tags[8], tos[8];
	uint32_t i, n, a_to_b = 0u, b_to_a = 0u;

	printf("-- loss is per DIRECTED link: A->B may fail while B->A works\n");
	lan_three_nodes(&lan, 2u);
	memset(&off, 0, sizeof(off));
	memset(&dead, 0, sizeof(dead));
	dead.loss_pct = 100u;
	sim_lan_set_link(&lan, 0u, 1u, &dead);   /* A -> B is dead   */
	sim_lan_set_link(&lan, 1u, 0u, &off);    /* B -> A is perfect */

	for (i = 0; i < 10u; i++) {
		(void)mkframe(f, mac_b, mac_a, 0x21);
		(void)sim_lan_xmit(&lan, 0u, f, 64u, 0u);
		a_to_b += drain(&lan, tags, tos, 8u);

		(void)mkframe(f, mac_a, mac_b, 0x22);
		(void)sim_lan_xmit(&lan, 1u, f, 64u, 0u);
		b_to_a += drain(&lan, tags, tos, 8u);
	}
	ct_check_eq_u32(a_to_b, 0, "nothing crossed A->B");
	ct_check_eq_u32(b_to_a, 10, "everything crossed B->A");
	ct_check_eq_u32((unsigned long)lan.lost, 10, "and the loss is counted");

	/* Partial loss: at 50 % over 200 frames, some but not all. */
	lan_three_nodes(&lan, 3u);
	memset(&dead, 0, sizeof(dead));
	dead.loss_pct = 50u;
	sim_lan_set_link(&lan, 0u, 1u, &dead);
	for (i = 0, n = 0u; i < 200u; i++) {
		(void)mkframe(f, mac_b, mac_a, (uint8_t)i);
		(void)sim_lan_xmit(&lan, 0u, f, 64u, 0u);
		n += drain(&lan, tags, tos, 8u);
	}
	ct_check(n > 60u && n < 140u, "50 % loss delivered a middling number");
	printf("       (delivered %u of 200)\n", n);
}

static void test_lan_duplication(void)
{
	struct sim_lan lan;
	struct sim_link cond;
	uint8_t f[64], tags[8], tos[8];
	uint32_t n;

	printf("-- a duplicate is the SAME bytes arriving twice\n");
	lan_three_nodes(&lan, 4u);
	memset(&cond, 0, sizeof(cond));
	cond.dup_pct = 100u;
	sim_lan_set_link(&lan, 0u, 1u, &cond);

	(void)mkframe(f, mac_b, mac_a, 0x33);
	(void)sim_lan_xmit(&lan, 0u, f, 64u, 0u);
	n = drain(&lan, tags, tos, 8u);
	ct_check_eq_u32(n, 2, "two copies arrived");
	ct_check(tags[0] == 0x33u && tags[1] == 0x33u, "with the same payload");
	ct_check_eq_u32((unsigned long)lan.duped, 1, "one duplication counted");
}

static void test_lan_reordering(void)
{
	struct sim_lan lan;
	struct sim_link hold, clean;
	uint8_t f[64], tags[8], tos[8];
	uint32_t n;

	printf("-- a reordered frame is OVERTAKEN by the one behind it\n");
	lan_three_nodes(&lan, 5u);
	memset(&hold, 0, sizeof(hold));
	memset(&clean, 0, sizeof(clean));
	hold.reorder_pct = 100u;
	hold.reorder_extra_ms = 5u;

	/* The first frame is held back 5 ms; the second goes on a clean link
	 * at the same instant, so it must arrive first. */
	sim_lan_set_link(&lan, 0u, 1u, &hold);
	(void)mkframe(f, mac_b, mac_a, 0x01);
	(void)sim_lan_xmit(&lan, 0u, f, 64u, 0u);

	sim_lan_set_link(&lan, 0u, 1u, &clean);
	(void)mkframe(f, mac_b, mac_a, 0x02);
	(void)sim_lan_xmit(&lan, 0u, f, 64u, 0u);

	n = drain(&lan, tags, tos, 8u);
	ct_check_eq_u32(n, 2, "both arrived");
	ct_check_eq_u32(tags[0], 0x02, "the SECOND frame arrived first");
	ct_check_eq_u32(tags[1], 0x01, "and the held-back one after it");
	ct_check_eq_u32((unsigned long)lan.reordered, 1, "one reorder counted");
}

static void test_lan_latency(void)
{
	struct sim_lan lan;
	struct sim_link slow;
	uint8_t f[64];
	uint32_t slot;

	printf("-- latency schedules the arrival, it does not drop it\n");
	lan_three_nodes(&lan, 6u);
	memset(&slow, 0, sizeof(slow));
	slow.latency_ms = 40u;
	sim_lan_set_link(&lan, 0u, 1u, &slow);

	(void)mkframe(f, mac_b, mac_a, 0x44);
	(void)sim_lan_xmit(&lan, 0u, f, 64u, 1000u);
	ct_check(sim_lan_next(&lan, &slot), "it is in flight");
	ct_check_eq_u32((unsigned long)sim_lan_peek(&lan, slot)->due_ms, 1040,
			"due at the transmit instant plus the one-way delay");
}

static void test_lan_multicast_is_group_membership(void)
{
	struct sim_lan lan;
	uint8_t f[64], tags[8], tos[8];
	uint32_t n;

	printf("-- a group frame reaches the group, not the sender, not "
	       "outsiders\n");
	sim_lan_init(&lan, 7u);
	(void)sim_lan_add_port(&lan, mac_a, group1);
	(void)sim_lan_add_port(&lan, mac_b, group1);
	(void)sim_lan_add_port(&lan, mac_c, group2);   /* a different cluster */

	(void)mkframe(f, group1, mac_a, 0x55);
	(void)sim_lan_xmit(&lan, 0u, f, 64u, 0u);
	n = drain(&lan, tags, tos, 8u);
	ct_check_eq_u32(n, 1, "exactly one port took it");
	ct_check_eq_u32(tos[0], 1, "the other member of group 1");

	/* And a port that joined no group at all takes only unicast. */
	sim_lan_init(&lan, 8u);
	(void)sim_lan_add_port(&lan, mac_a, group1);
	(void)sim_lan_add_port(&lan, mac_b, NULL);
	(void)mkframe(f, group1, mac_a, 0x56);
	(void)sim_lan_xmit(&lan, 0u, f, 64u, 0u);
	ct_check_eq_u32(drain(&lan, tags, tos, 8u), 0,
			"a port in no group hears no group frame");
	ct_check_eq_u32((unsigned long)lan.undeliverable, 1,
			"and the frame is counted undeliverable, not dropped "
			"silently");
}

static void test_lan_cut_and_down_port(void)
{
	struct sim_lan lan;
	uint8_t f[64], tags[8], tos[8];

	printf("-- a partition and a down NIC are separate, counted facts\n");
	lan_three_nodes(&lan, 9u);
	sim_lan_cut(&lan, 0u, 1u, 1);
	(void)mkframe(f, mac_b, mac_a, 0x66);
	(void)sim_lan_xmit(&lan, 0u, f, 64u, 0u);
	ct_check_eq_u32(drain(&lan, tags, tos, 8u), 0, "nothing crossed a cut");
	ct_check_eq_u32((unsigned long)lan.cut_blocked, 1, "counted as a cut");
	ct_check_eq_u32((unsigned long)lan.lost, 0,
			"and NOT counted as loss -- they are different facts");

	sim_lan_cut(&lan, 0u, 1u, 0);
	sim_lan_set_up(&lan, 1u, 0);
	(void)sim_lan_xmit(&lan, 0u, f, 64u, 0u);
	ct_check_eq_u32(drain(&lan, tags, tos, 8u), 0,
			"nothing reached a port whose NIC is down");
	ct_check_eq_u32((unsigned long)lan.link_down_blocked, 1,
			"counted as a down link");

	sim_lan_set_up(&lan, 1u, 1);
	(void)sim_lan_xmit(&lan, 0u, f, 64u, 0u);
	ct_check_eq_u32(drain(&lan, tags, tos, 8u), 1,
			"and it flows again once the NIC comes back");
}

static void test_lan_replay_is_identical(void)
{
	struct sim_lan a, b;
	struct sim_link cond;
	uint8_t f[64], ta[64], tb[64], oa[64], ob[64];
	uint32_t i, na = 0u, nb = 0u;
	int identical = 1;

	printf("-- the same seed replays the same wire, frame for frame\n");
	memset(&cond, 0, sizeof(cond));
	cond.loss_pct = 10u;
	cond.dup_pct = 5u;
	cond.reorder_pct = 5u;

	lan_three_nodes(&a, 4242u);
	lan_three_nodes(&b, 4242u);
	sim_lan_set_link_all(&a, &cond);
	sim_lan_set_link_all(&b, &cond);
	for (i = 0; i < 40u; i++) {
		(void)mkframe(f, group1, mac_a, (uint8_t)i);
		(void)sim_lan_xmit(&a, 0u, f, 64u, i);
		(void)sim_lan_xmit(&b, 0u, f, 64u, i);
	}
	na = drain(&a, ta, oa, 64u);
	nb = drain(&b, tb, ob, 64u);
	if (na != nb || memcmp(ta, tb, na) != 0 || memcmp(oa, ob, na) != 0)
		identical = 0;
	ct_check(identical, "identical delivery sequences");
	ct_check(a.lost == b.lost && a.duped == b.duped &&
		 a.reordered == b.reordered, "identical counters");
	ct_check(a.lost > 0u, "and the conditions actually fired");
	printf("       (%u deliveries, %llu lost, %llu duped, %llu reordered)\n",
	       na, (unsigned long long)a.lost, (unsigned long long)a.duped,
	       (unsigned long long)a.reordered);
}

int main(void)
{
	printf("test_sim_lan: the rung-2 harness's own model (FC-P1.4)\n");

	test_rng_determinism();
	test_rng_streams_are_independent();
	test_rng_pct();

	test_clock_arm_moves_never_stacks();
	test_clock_cancel();
	test_clock_order_is_total();
	test_clock_two_time_bases();

	test_lan_perfect_wire();
	test_lan_loss_and_asymmetry();
	test_lan_duplication();
	test_lan_reordering();
	test_lan_latency();
	test_lan_multicast_is_group_membership();
	test_lan_cut_and_down_port();
	test_lan_replay_is_identical();

	return ct_summary("test_sim_lan");
}
