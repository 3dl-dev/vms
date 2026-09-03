/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cnxman_fake_ops.h - the injected struct cnxman_ops the FC-P3.6 host tests
 * drive the connection manager with.
 *
 * THE CLOCK IS A VARIABLE. Design SS3.9 rule 6: "Deadlines and identities are
 * injected (ops.now, params), never read from the substrate inside an FSM -- so
 * a test drives time." Every test here sets `f.now_ms` and calls; a twenty-
 * second RECNXINTERVAL window therefore runs in microseconds and is exactly
 * reproducible, which is the entire reason the reconnect apparatus is testable
 * at rung 1 instead of only against a live VAX at rung 5.
 *
 * `send`, `respond`, `alloc` and `free` are deliberately left NULL. Neither the
 * CSB ladder nor the reconnect loop may transmit or allocate -- they decide, and
 * hand the decision back as an action. A NULL function pointer is a harder
 * assertion of that than any counter: if either layer ever reaches for the wire,
 * these tests crash instead of quietly passing.
 */
#ifndef OVMX_CNXMAN_FAKE_OPS_H
#define OVMX_CNXMAN_FAKE_OPS_H

#include <string.h>

#include "vms_cluster.h"
#include "vms_cnxman.h"

struct fake_cnx {
	uint32_t now_ms;            /* the injected clock */
	uint32_t timers_armed;
	uint32_t timers_cancelled;
	uint32_t last_arm_ms;
	uint32_t last_arm_which;
	uint32_t logs;
	char     last_log[160];
};

static uint32_t fake_now_ms(void *ctx)
{
	return ((struct fake_cnx *)ctx)->now_ms;
}

static void fake_arm_timer(void *ctx, enum cnxman_timer which, uint32_t key,
			   uint32_t ms)
{
	struct fake_cnx *f = (struct fake_cnx *)ctx;

	(void)key;
	f->timers_armed++;
	f->last_arm_ms = ms;
	f->last_arm_which = (uint32_t)which;
}

static void fake_cancel_timer(void *ctx, enum cnxman_timer which, uint32_t key)
{
	struct fake_cnx *f = (struct fake_cnx *)ctx;

	(void)which;
	(void)key;
	f->timers_cancelled++;
}

static void fake_log(void *ctx, const char *msg)
{
	struct fake_cnx *f = (struct fake_cnx *)ctx;

	f->logs++;
	if (msg != NULL) {
		size_t n = strlen(msg);

		if (n >= sizeof(f->last_log))
			n = sizeof(f->last_log) - 1;
		memcpy(f->last_log, msg, n);
		f->last_log[n] = '\0';
	}
}

static void fake_ops_init(struct cnxman_ops *ops, struct fake_cnx *f)
{
	memset(f, 0, sizeof(*f));
	memset(ops, 0, sizeof(*ops));
	ops->arm_timer = fake_arm_timer;
	ops->cancel_timer = fake_cancel_timer;
	ops->now_ms = fake_now_ms;
	ops->log = fake_log;
	ops->ctx = f;
}

#endif /* OVMX_CNXMAN_FAKE_OPS_H */
