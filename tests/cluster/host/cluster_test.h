/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cluster_test.h - the two-line check harness shared by the host cluster
 * codec tests (FC-P0.6). Deliberately tiny: no framework, no fixtures magic,
 * and every failure prints what was expected and what was seen, because a
 * test that only prints "FAIL" costs a debugging round trip.
 */
#ifndef OVMX_CLUSTER_TEST_H
#define OVMX_CLUSTER_TEST_H

#include <stdio.h>

static int ct_checks;
static int ct_failures;

static void ct_check(int cond, const char *what)
{
	ct_checks++;
	if (cond) {
		printf("  ok   %s\n", what);
	} else {
		printf("  FAIL %s\n", what);
		ct_failures++;
	}
}

static void ct_check_eq_u32(unsigned long got, unsigned long want,
			    const char *what) __attribute__((unused));
static void ct_check_eq_u32(unsigned long got, unsigned long want,
			    const char *what)
{
	ct_checks++;
	if (got == want) {
		printf("  ok   %s (%lu)\n", what, got);
	} else {
		printf("  FAIL %s: got %lu (0x%lx), want %lu (0x%lx)\n",
		       what, got, got, want, want);
		ct_failures++;
	}
}

static int ct_summary(const char *suite)
{
	printf("%s: %d checks, %d failures\n", suite, ct_checks, ct_failures);
	return ct_failures == 0 ? 0 : 1;
}

#endif /* OVMX_CLUSTER_TEST_H */
