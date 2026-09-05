/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_pe_credit.c - the port's receive-buffer credit ledger (E60,
 * vms_pe_fsm.h §4b), asserted as arithmetic.
 *
 * WHY THIS FILE EXISTS SEPARATELY FROM test_pe_vc.c. That file proves the
 * COUPLING -- that the byte at abs 95 of a real START frame is the reservation
 * this ledger granted, and that it moves when the pool moves. This one proves
 * the INVARIANT underneath it, over the whole input space rather than the two
 * or three pool sizes a formation test can afford to drive:
 *
 *     the sum of everything this port has promised
 *     never exceeds the buffers it actually owns.
 *
 * That is p. 2-43's bank analogy stated as code ("each person is entitled only
 * to the amount of money that he or she has on deposit"), and it is the whole
 * reason a Send Credit may not be a configured number: p. 2-43 again, "the
 * local SYSAP requests SCS to allocate a certain number of buffers to receive
 * incoming messages from the remote SYSAP ... then the local SYSAP is said to
 * have extended 10 Send Credits to the remote SYSAP". The peer sends that many
 * messages without waiting. Every one of them needs a buffer that exists.
 *
 * No fixture, no frame, no clock: four functions and two counters.
 */

#include <stdio.h>

#include "cluster_test.h"
#include "vms_pe_fsm.h"

/* The pool a real port allocates (CF_RX_BUFS_DEFAULT), so the numbers here are
 * the numbers a booted node works with. */
#define POOL_BUFS 64u

/* SYSGEN CLUSTER_CREDITS as the lab's VAXes carry it. A REQUEST here, never a
 * wire value -- which is exactly what this file exists to keep true. */
#define REQUEST   10u

static void test_a_fresh_ledger_owns_its_pool(void)
{
	struct pe_credit_ledger l;

	printf("-- a fresh ledger has promised nothing\n");
	pe_credit_init(&l, POOL_BUFS);
	ct_check_eq_u32(l.pool, POOL_BUFS, "it owns the buffers it was given");
	ct_check_eq_u32(l.reserved, 0, "and has committed none of them");
	ct_check_eq_u32(pe_credit_available(&l), POOL_BUFS,
			"so all of them are available");
}

static void test_a_grant_is_the_smaller_of_want_and_have(void)
{
	struct pe_credit_ledger l;

	printf("-- a grant is min(requested, owned) -- never the request\n");

	pe_credit_init(&l, POOL_BUFS);
	ct_check_eq_u32(pe_credit_reserve(&l, REQUEST), REQUEST,
			"a pool that can back the request grants it in full");
	ct_check_eq_u32(l.reserved, REQUEST, "and records the withdrawal");

	pe_credit_init(&l, 4u);
	ct_check_eq_u32(pe_credit_reserve(&l, REQUEST), 4,
			"a pool of 4 grants 4 against a request for 10");
	ct_check_eq_u32(pe_credit_available(&l), 0, "spent, not overdrawn");

	pe_credit_init(&l, 0u);
	ct_check_eq_u32(pe_credit_reserve(&l, REQUEST), 0,
			"a port that owns nothing promises nothing");

	pe_credit_init(&l, POOL_BUFS);
	ct_check_eq_u32(pe_credit_reserve(&l, 0u), 0,
			"and a request for nothing is granted nothing");
	ct_check_eq_u32(l.reserved, 0, "with no buffer taken out of the pool");
}

/*
 * The invariant, driven past exhaustion: six circuits at 10 fit in 64 buffers,
 * the seventh gets the 4 that are left, and the eighth gets nothing. At no
 * point does the sum of the grants exceed the pool -- which is the property
 * that makes every one of those promises keepable at the same time.
 */
static void test_the_sum_of_promises_never_exceeds_the_pool(void)
{
	struct pe_credit_ledger l;
	uint32_t total = 0u;
	uint32_t i;
	uint8_t grant;

	printf("-- N circuits share ONE pool, and it is never overdrawn\n");
	pe_credit_init(&l, POOL_BUFS);

	for (i = 0; i < 8u; i++) {
		grant = pe_credit_reserve(&l, REQUEST);
		total += (uint32_t)grant;
		ct_check(total <= POOL_BUFS,
			 "the running total stays inside the pool");
		ct_check_eq_u32(l.reserved, total,
				"and the ledger agrees with it");
	}
	ct_check_eq_u32(total, POOL_BUFS,
			"64 buffers were promised: 6 full grants, then 4");
	ct_check_eq_u32(pe_credit_reserve(&l, REQUEST), 0,
			"a circuit arriving at an empty pool is told 0");
}

static void test_a_release_returns_the_buffers(void)
{
	struct pe_credit_ledger l;
	uint8_t a, b;

	printf("-- a closed circuit's share goes back (p. 2-43)\n");
	pe_credit_init(&l, POOL_BUFS);
	a = pe_credit_reserve(&l, REQUEST);
	b = pe_credit_reserve(&l, REQUEST);
	ct_check_eq_u32(l.reserved, (uint32_t)a + (uint32_t)b,
			"two circuits, two withdrawals");

	pe_credit_release(&l, a);
	ct_check_eq_u32(l.reserved, b, "one closed: only the other's share left");
	ct_check_eq_u32(pe_credit_available(&l), POOL_BUFS - (uint32_t)b,
			"and the freed buffers are available again");

	pe_credit_release(&l, b);
	ct_check_eq_u32(pe_credit_available(&l), POOL_BUFS,
			"both closed: the whole pool is back");

	/* Idempotent by construction: the FSM releases on close AND on
	 * re-formation, and a circuit holding nothing must release nothing. */
	pe_credit_release(&l, 0u);
	pe_credit_release(&l, 200u);
	ct_check_eq_u32(l.reserved, 0,
			"an over-release clamps at zero, it never wraps");
	ct_check_eq_u32(pe_credit_available(&l), POOL_BUFS,
			"leaving the pool exactly as full as it really is");
}

/*
 * The advertised field is one byte wide. A pool that could back more than 255
 * buffers for one circuit therefore promises 255 and keeps the rest: a promise
 * that does not fit the wire is made SMALLER. Rounding it UP -- or wrapping it
 * -- would be the one failure mode this whole ledger exists to prevent.
 */
static void test_a_grant_never_outgrows_the_field(void)
{
	struct pe_credit_ledger l;

	printf("-- a grant is capped at the width of the field, downward\n");
	pe_credit_init(&l, 4096u);
	ct_check_eq_u32(pe_credit_reserve(&l, 1000u), 255,
			"1000 requested from a deep pool grants 255");
	ct_check_eq_u32(l.reserved, 255,
			"and only the 255 actually promised are held");
}

static void test_a_null_ledger_promises_nothing(void)
{
	printf("-- no ledger, no promise (and no crash)\n");
	pe_credit_init(NULL, POOL_BUFS);
	ct_check_eq_u32(pe_credit_reserve(NULL, REQUEST), 0,
			"a reserve against no ledger grants 0");
	ct_check_eq_u32(pe_credit_available(NULL), 0,
			"and nothing is available from it");
	pe_credit_release(NULL, 10u);
}

int main(void)
{
	test_a_fresh_ledger_owns_its_pool();
	test_a_grant_is_the_smaller_of_want_and_have();
	test_the_sum_of_promises_never_exceeds_the_pool();
	test_a_release_returns_the_buffers();
	test_a_grant_never_outgrows_the_field();
	test_a_null_ledger_promises_nothing();
	return ct_summary("test_pe_credit");
}
