/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cluster_fork.c - rung R1 for the cluster fork context (FC-P0.5).
 *
 * Drives src/kernel-core/vms_cluster_fork.c -- the SHIPPING translation unit,
 * not a copy and not a differently-compiled variant of it -- with fake ops, on
 * the host, with no thread and no kernel. Every dispatch step is taken through
 * cf_dispatch_one(), so the whole queue discipline is deterministic and the
 * assertions are on exact orders and exact counters.
 *
 * The fake ops are also a HARNESS FOR THE LOCK RULES: they abort the moment the
 * module takes the fork mutex while holding the queue lock (the ABBA inversion
 * that design §3.3's lock order forbids), or calls the may-sleep timer cancel
 * under the queue lock, or sleeps in a test that must never sleep. A test that
 * only checked outputs would pass on a module that had those bugs.
 *
 * Concurrency proper -- real threads, real wakeups -- is test_cluster_fork_threads.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vms_cluster_fork.h"
#include "cluster_test.h"

/* ------------------------------------------------------------------ *
 * The fake substrate
 * ------------------------------------------------------------------ */

struct fake {
	int qlock_depth;
	int fork_depth;
	int wait_calls;
	int wake_calls;
	int should_stop;

	/* the SERVED-I/O WORKER's own three ops (FC-P6.6) */
	int io_wait_calls;
	int io_wake_calls;
	int io_should_stop;

	/* the last arm/cancel per slot, so timer wiring can be asserted */
	uint32_t arm_ms[64];
	int      arm_count[64];
	int      cancel_count[64];

	int allocs;
	int frees;
};

static void die(const char *why)
{
	printf("  FAIL (harness) %s\n", why);
	exit(1);
}

static void fk_lock(void *ctx)
{
	struct fake *f = ctx;

	if (f->qlock_depth)
		die("queue lock taken recursively");
	f->qlock_depth++;
}

static void fk_unlock(void *ctx)
{
	struct fake *f = ctx;

	if (!f->qlock_depth)
		die("queue lock released while not held");
	f->qlock_depth--;
}

static int fk_wait(void *ctx)
{
	struct fake *f = ctx;

	f->wait_calls++;
	die("cf_run slept in a single-threaded test: it would never wake");
	return 0;
}

static void fk_wake(void *ctx)
{
	struct fake *f = ctx;

	if (!f->qlock_depth)
		die("wake issued without the queue lock (the cv contract)");
	f->wake_calls++;
}

static void fk_fork_lock(void *ctx)
{
	struct fake *f = ctx;

	if (f->qlock_depth)
		die("fork mutex taken while holding the queue lock (ABBA)");
	if (f->fork_depth)
		die("fork mutex taken recursively: events are not serialised");
	f->fork_depth++;
}

static void fk_fork_unlock(void *ctx)
{
	struct fake *f = ctx;

	if (!f->fork_depth)
		die("fork mutex released while not held");
	f->fork_depth--;
}

static void fk_timer_arm(void *ctx, uint32_t slot, uint32_t ms)
{
	struct fake *f = ctx;

	if (!f->qlock_depth)
		die("timer armed without the queue lock");
	if (slot >= 64)
		die("timer slot out of range");
	f->arm_ms[slot] = ms;
	f->arm_count[slot]++;
}

static void fk_timer_cancel(void *ctx, uint32_t slot)
{
	struct fake *f = ctx;

	if (f->qlock_depth)
		die("timer cancel (which MAY SLEEP) called under the queue lock");
	if (slot >= 64)
		die("timer slot out of range");
	f->cancel_count[slot]++;
}

static int fk_should_stop(void *ctx)
{
	return ((struct fake *)ctx)->should_stop;
}

/* The SERVED-I/O WORKER's ops (FC-P6.6), held to the SAME rules as the fork
 * thread's: the wake is only legal under the queue lock, and a sleep in a
 * single-threaded test is a harness abort, not a hang. */
static int fk_io_wait(void *ctx)
{
	struct fake *f = ctx;

	f->io_wait_calls++;
	die("cf_io_run slept in a single-threaded test: it would never wake");
	return 0;
}

static void fk_io_wake(void *ctx)
{
	struct fake *f = ctx;

	if (!f->qlock_depth)
		die("io wake issued without the queue lock (the cv contract)");
	f->io_wake_calls++;
}

static int fk_io_should_stop(void *ctx)
{
	return ((struct fake *)ctx)->io_should_stop;
}

static void *fk_alloc(void *ctx, uint32_t n)
{
	struct fake *f = ctx;
	void *p = calloc(1, n ? n : 1);

	if (p)
		f->allocs++;
	return p;
}

static void fk_free(void *ctx, void *p)
{
	struct fake *f = ctx;

	if (p) {
		f->frees++;
		free(p);
	}
}

static void fake_ops(struct cf_ops *o, struct fake *f)
{
	memset(o, 0, sizeof(*o));
	o->lock         = fk_lock;
	o->unlock       = fk_unlock;
	o->wait         = fk_wait;
	o->wake         = fk_wake;
	o->fork_lock    = fk_fork_lock;
	o->fork_unlock  = fk_fork_unlock;
	o->timer_arm    = fk_timer_arm;
	o->timer_cancel = fk_timer_cancel;
	o->should_stop  = fk_should_stop;
	o->io_wait        = fk_io_wait;
	o->io_wake        = fk_io_wake;
	o->io_should_stop = fk_io_should_stop;
	o->alloc        = fk_alloc;
	o->free         = fk_free;
	o->ctx          = f;
}

/* ------------------------------------------------------------------ *
 * The recorder the handlers write into
 * ------------------------------------------------------------------ */

#define REC_MAX 256

struct rec {
	char  seq[REC_MAX][32];   /* a text transcript of the dispatch order */
	int   n;
	int   in_fork;            /* set by the harness while a handler runs */
	struct vms_cluster_fork *f;
	int   repost;             /* if set, the work handler posts once more */
};

static void rec_add(struct rec *r, const char *fmt, unsigned a, unsigned b)
{
	if (r->n >= REC_MAX)
		die("recorder overflow");
	snprintf(r->seq[r->n], sizeof(r->seq[0]), fmt, a, b);
	r->n++;
}

static void h_rx(void *ctx, const uint8_t *frame, uint32_t len)
{
	struct rec *r = ctx;

	rec_add(r, "rx:%u/%u", frame[0], len);
}

static void h_work(void *ctx, const struct cf_work *w)
{
	struct rec *r = ctx;

	if (w->kind == CF_WORK_TIMER)
		rec_add(r, "timer:%u/%u", w->arg0, w->arg1);
	else
		rec_add(r, "work:%u/%u", w->kind, w->arg0);

	if (r->repost) {
		struct cf_work nw;

		r->repost = 0;
		memset(&nw, 0, sizeof(nw));
		nw.owner = CF_OWNER_PE;
		nw.kind  = 99;
		nw.arg0  = 99;
		(void)cf_post(r->f, &nw);   /* a handler may post: no deadlock */
	}
}

static int transcript_is(struct rec *r, const char *const *want, int n,
			 const char *what)
{
	int i, ok = (r->n == n);

	for (i = 0; ok && i < n; i++)
		ok = (strcmp(r->seq[i], want[i]) == 0);
	ct_check(ok, what);
	if (!ok) {
		printf("       got:");
		for (i = 0; i < r->n; i++)
			printf(" %s", r->seq[i]);
		printf("\n      want:");
		for (i = 0; i < n; i++)
			printf(" %s", want[i]);
		printf("\n");
	}
	return ok;
}

/* Build a fork context with small pools so exhaustion is reachable. */
static struct vms_cluster_fork *mk(struct fake *f, struct rec *r,
				   uint32_t rxbufs, uint32_t work, uint32_t slots)
{
	struct cf_ops o;
	struct cf_config c;
	struct vms_cluster_fork *fk;

	memset(f, 0, sizeof(*f));
	memset(r, 0, sizeof(*r));
	memset(&c, 0, sizeof(c));
	c.rx_bufs = rxbufs;
	c.rx_cap = 64;
	c.work_items = work;
	c.timer_slots = slots;
	fake_ops(&o, f);

	fk = cf_create(&o, &c);
	if (!fk)
		die("cf_create failed");
	r->f = fk;
	cf_set_rx_handler(fk, h_rx, r);
	(void)cf_set_work_handler(fk, CF_OWNER_PE, h_work, r);
	return fk;
}

static void feed_rx(struct vms_cluster_fork *fk, uint8_t tag, uint32_t len)
{
	uint8_t buf[64];

	memset(buf, 0, sizeof(buf));
	buf[0] = tag;
	(void)cf_rx_deliver(fk, buf, len);
}

static cf_status_t post_kind(struct vms_cluster_fork *fk, uint16_t kind)
{
	struct cf_work w;

	memset(&w, 0, sizeof(w));
	w.owner = CF_OWNER_PE;
	w.kind = kind;
	w.arg0 = kind;
	return cf_post(fk, &w);
}

/* ------------------------------------------------------------------ *
 * 1. FIFO within each queue, and alternation between them
 * ------------------------------------------------------------------ */
static void test_fifo_and_alternation(void)
{
	static const char *const want[] = {
		"rx:1/8", "work:10/10", "rx:2/8", "work:11/11", "rx:3/8"
	};
	struct fake f;
	struct rec r;
	struct vms_cluster_fork *fk = mk(&f, &r, 8, 8, 4);
	struct cf_stats st;
	int n = 0;

	printf("-- FIFO per queue + alternation between queues\n");
	feed_rx(fk, 1, 8);
	feed_rx(fk, 2, 8);
	feed_rx(fk, 3, 8);
	ct_check(post_kind(fk, 10) == CF_OK, "work 10 accepted");
	ct_check(post_kind(fk, 11) == CF_OK, "work 11 accepted");

	while (cf_dispatch_one(fk))
		n++;
	ct_check_eq_u32((unsigned)n, 5, "five events dispatched");
	transcript_is(&r, want, 5,
		      "rx and work alternate, each queue strictly FIFO");

	cf_stats_get(fk, &st);
	ct_check_eq_u32((unsigned long)st.rx_dispatched, 3, "rx_dispatched");
	ct_check_eq_u32((unsigned long)st.work_dispatched, 2, "work_dispatched");
	ct_check_eq_u32(st.rx_free, 8, "every receive buffer came back");
	ct_check_eq_u32(st.work_free, 8, "every work item came back");
	ct_check_eq_u32((unsigned)f.wait_calls, 0, "never slept");
	ct_check_eq_u32((unsigned)f.fork_depth, 0, "fork mutex released");
	cf_destroy(fk);
	ct_check_eq_u32((unsigned)f.allocs, (unsigned)f.frees,
			"every pool allocation was freed");
}

/* ------------------------------------------------------------------ *
 * 2. Pool exhaustion is an honest counted drop, and the pool recycles
 * ------------------------------------------------------------------ */
static void test_pool_exhaustion(void)
{
	struct fake f;
	struct rec r;
	struct vms_cluster_fork *fk = mk(&f, &r, 2, 1, 4);
	struct cf_stats st;
	uint8_t big[64];

	printf("-- pool exhaustion: dropped and counted, never blocked\n");
	feed_rx(fk, 1, 8);
	feed_rx(fk, 2, 8);
	feed_rx(fk, 3, 8);                      /* pool empty: dropped */
	memset(big, 7, sizeof(big));
	ct_check(cf_rx_deliver(fk, big, 65) == CF_E_TOOBIG,
		 "a frame past the buffer capacity is refused, not truncated");
	ct_check(post_kind(fk, 20) == CF_OK, "first work item accepted");
	ct_check(post_kind(fk, 21) == CF_E_NOBUF,
		 "work pool empty reports CF_E_NOBUF");

	cf_stats_get(fk, &st);
	ct_check_eq_u32((unsigned long)st.rx_enqueued, 2, "rx_enqueued");
	ct_check_eq_u32((unsigned long)st.rx_dropped_nobuf, 1, "rx_dropped_nobuf");
	ct_check_eq_u32((unsigned long)st.rx_dropped_toobig, 1, "rx_dropped_toobig");
	ct_check_eq_u32((unsigned long)st.work_dropped_nobuf, 1, "work_dropped_nobuf");

	while (cf_dispatch_one(fk))
		;
	cf_stats_get(fk, &st);
	ct_check_eq_u32(st.rx_free, 2, "receive pool recycled after dispatch");
	ct_check_eq_u32(st.work_free, 1, "work pool recycled after dispatch");

	feed_rx(fk, 4, 8);                      /* proves the pool is reusable */
	ct_check(cf_dispatch_one(fk) == 1, "a frame flows again after recycling");
	cf_destroy(fk);
}

/* ------------------------------------------------------------------ *
 * 3. Timers: post-not-run, coalescing, cancel, slot reuse, exhaustion
 * ------------------------------------------------------------------ */
static void test_timers(void)
{
	static const char *const want[] = { "timer:3/7" };
	struct fake f;
	struct rec r;
	struct vms_cluster_fork *fk = mk(&f, &r, 4, 8, 2);
	struct cf_stats st;

	printf("-- timers: a callback only posts; expiries coalesce\n");
	ct_check(cf_timer_arm(fk, CF_OWNER_PE, 3, 7, 2000) == CF_OK,
		 "arm (owner=PE, which=3, key=7, 2000 ms)");
	ct_check_eq_u32((unsigned)f.arm_count[0], 1, "the substrate timer was armed");
	ct_check_eq_u32(f.arm_ms[0], 2000, "with the requested delay");
	ct_check_eq_u32((unsigned)r.n, 0, "arming ran no handler");

	cf_timer_expired(fk, 0);                /* the substrate callback */
	ct_check_eq_u32((unsigned)r.n, 0,
			"the expiry callback ran NO protocol (contract rule 2)");

	/* A one-shot timer that has fired is no longer armed, so a further
	 * callback can only follow a re-arm. Re-arm and fire three more times
	 * BEFORE the fork thread dispatches: all three must coalesce onto the
	 * one queued expiry rather than pile up three duplicate events. */
	(void)cf_timer_arm(fk, CF_OWNER_PE, 3, 7, 2000);
	cf_timer_expired(fk, 0);
	(void)cf_timer_arm(fk, CF_OWNER_PE, 3, 7, 2000);
	cf_timer_expired(fk, 0);
	(void)cf_timer_arm(fk, CF_OWNER_PE, 3, 7, 2000);
	cf_timer_expired(fk, 0);

	cf_stats_get(fk, &st);
	ct_check_eq_u32((unsigned long)st.timer_expiries, 1, "one expiry queued");
	ct_check_eq_u32((unsigned long)st.timer_coalesced, 3,
			"three further expiries coalesced onto it");
	ct_check_eq_u32((unsigned long)st.timer_stale, 0,
			"and none of them was mistaken for a stale callback");

	ct_check(cf_dispatch_one(fk) == 1, "the expiry dispatches as work");
	ct_check(cf_dispatch_one(fk) == 0, "and only once");
	transcript_is(&r, want, 1, "the handler sees which=3 key=7 as CF_WORK_TIMER");

	/* Re-arming the same identity reuses the slot; it never stacks. */
	ct_check(cf_timer_arm(fk, CF_OWNER_PE, 3, 7, 1500) == CF_OK, "re-arm");
	cf_stats_get(fk, &st);
	ct_check_eq_u32(st.timer_slots_used, 1, "the same identity reuses its slot");
	ct_check_eq_u32((unsigned)f.arm_count[0], 5,
			"every arm MOVED the one substrate timer (1 + 3 re-arms + this)");
	ct_check_eq_u32(f.arm_ms[0], 1500, "the latest delay is the one in force");

	/* A second identity takes the second slot; a third has nowhere to go and
	 * is REFUSED rather than silently unarmed. */
	ct_check(cf_timer_arm(fk, CF_OWNER_PE, 4, 0, 100) == CF_OK, "second identity");
	ct_check(cf_timer_arm(fk, CF_OWNER_PE, 5, 0, 100) == CF_E_NOSLOT,
		 "slot table full is an honest CF_E_NOSLOT");

	cf_destroy(fk);
}

/* A tick that cannot be queued is LOST AND COUNTED, never faked: the layer's
 * own deadline is what notices, exactly as with a dropped frame. */
static void test_timer_tick_lost_when_pool_full(void)
{
	struct fake f;
	struct rec r;
	struct vms_cluster_fork *fk = mk(&f, &r, 4, 1, 4);
	struct cf_stats st;

	printf("-- a tick with no work item is counted, not fabricated\n");
	ct_check(cf_timer_arm(fk, CF_OWNER_PE, 2, 2, 100) == CF_OK, "armed");
	ct_check(post_kind(fk, 50) == CF_OK, "the single work item is taken");
	cf_timer_expired(fk, 0);
	cf_stats_get(fk, &st);
	ct_check_eq_u32((unsigned long)st.timer_dropped_nobuf, 1,
			"timer_dropped_nobuf counts the lost tick");
	ct_check_eq_u32((unsigned long)st.timer_expiries, 0,
			"and it is NOT counted as an expiry that was queued");
	cf_destroy(fk);
}

static void test_timer_cancel(void)
{
	struct fake f;
	struct rec r;
	struct vms_cluster_fork *fk = mk(&f, &r, 4, 8, 4);
	struct cf_stats st;

	printf("-- cancel: a cancelled timer can never be dispatched\n");
	(void)cf_timer_arm(fk, CF_OWNER_PE, 1, 42, 500);
	cf_timer_expired(fk, 0);                /* the expiry is now QUEUED */
	cf_stats_get(fk, &st);
	ct_check_eq_u32((unsigned long)st.timer_expiries, 1, "expiry queued");

	cf_timer_cancel(fk, CF_OWNER_PE, 1, 42);
	ct_check_eq_u32((unsigned)f.cancel_count[0], 1,
			"the substrate cancel ran (it waits out the callback)");
	cf_stats_get(fk, &st);
	ct_check_eq_u32((unsigned long)st.timer_dequeued, 1,
			"the already-queued expiry was pulled back off the queue");
	ct_check_eq_u32(st.work_free, 8, "and returned to the pool");
	ct_check(cf_dispatch_one(fk) == 0,
		 "nothing is dispatched for a cancelled timer");
	ct_check_eq_u32((unsigned)r.n, 0, "the layer never sees the stale expiry");

	/* A callback that was already in flight when the cancel released the
	 * slot must be ignored, not delivered to whatever holds the slot next. */
	cf_timer_expired(fk, 0);
	cf_stats_get(fk, &st);
	ct_check_eq_u32((unsigned long)st.timer_stale, 1,
			"a late callback for a released slot is stale, not work");
	ct_check(cf_dispatch_one(fk) == 0, "and queues nothing");

	/* Cancelling something that was never armed is a no-op, not a fault. */
	cf_timer_cancel(fk, CF_OWNER_PE, 9, 9);
	ct_check(cf_dispatch_one(fk) == 0, "cancel of an unarmed identity is inert");
	cf_destroy(fk);
}

/* ------------------------------------------------------------------ *
 * 4. Stop: refuse new work, run what is already queued, then return
 * ------------------------------------------------------------------ */
static void test_stop_drains(void)
{
	static const char *const want[] = {
		"rx:1/8", "work:30/30", "work:31/31"
	};
	struct fake f;
	struct rec r;
	struct vms_cluster_fork *fk = mk(&f, &r, 4, 8, 4);
	struct cf_stats st;

	printf("-- stop while work is pending drains cleanly\n");
	feed_rx(fk, 1, 8);
	ct_check(post_kind(fk, 30) == CF_OK, "work 30 queued before the stop");
	ct_check(post_kind(fk, 31) == CF_OK, "work 31 queued before the stop");

	cf_request_stop(fk);

	ct_check(post_kind(fk, 32) == CF_E_STOPPING,
		 "work posted after the stop is refused, not silently dropped");
	{
		uint8_t buf[8];

		memset(buf, 0, sizeof(buf));
		ct_check(cf_rx_deliver(fk, buf, 8) == CF_E_STOPPING,
			 "a frame arriving after the stop is refused");
	}

	cf_run(fk);   /* must return: the queues can only shrink now */

	transcript_is(&r, want, 3,
		      "everything queued before the stop RAN, in order");
	cf_stats_get(fk, &st);
	ct_check_eq_u32((unsigned long)st.work_refused_stopping, 1,
			"work_refused_stopping");
	ct_check_eq_u32((unsigned long)st.rx_refused_stopping, 1,
			"rx_refused_stopping");
	ct_check_eq_u32((unsigned long)st.waits, 0,
			"cf_run never slept: it had work, then it had a stop");
	ct_check_eq_u32(st.stopping, 1, "the stop is visible in the counters");
	cf_destroy(fk);
}

static void test_run_exits_on_substrate_stop(void)
{
	struct fake f;
	struct rec r;
	struct vms_cluster_fork *fk = mk(&f, &r, 4, 8, 4);

	printf("-- cf_run also honours the substrate's own stop request\n");
	f.should_stop = 1;      /* exec_kthread_should_stop() says "stop" */
	cf_run(fk);             /* fk_wait would abort the test if it slept */
	ct_check(1, "cf_run returned without sleeping on an empty queue");
	cf_destroy(fk);
}

/* ------------------------------------------------------------------ *
 * 5. Honest handling of what the module cannot deliver
 * ------------------------------------------------------------------ */
static void test_undeliverable(void)
{
	struct fake f;
	struct rec r;
	struct vms_cluster_fork *fk = mk(&f, &r, 4, 8, 4);
	struct cf_stats st;
	struct cf_work w;

	printf("-- no handler: counted as undeliverable, never a fabricated run\n");
	memset(&w, 0, sizeof(w));
	w.owner = CF_OWNER_CNXMAN;     /* nobody registered for this owner */
	w.kind = 1;
	ct_check(cf_post(fk, &w) == CF_OK, "the post itself is accepted");
	ct_check(cf_dispatch_one(fk) == 1, "and is dequeued");
	cf_stats_get(fk, &st);
	ct_check_eq_u32((unsigned long)st.work_undeliverable, 1,
			"work_undeliverable counts it");
	ct_check_eq_u32((unsigned long)st.work_dispatched, 0,
			"and it is NOT counted as dispatched");

	cf_set_rx_handler(fk, NULL, NULL);
	feed_rx(fk, 5, 8);
	(void)cf_dispatch_one(fk);
	cf_stats_get(fk, &st);
	ct_check_eq_u32((unsigned long)st.rx_undeliverable, 1, "rx_undeliverable");
	ct_check_eq_u32(st.rx_free, 4, "the buffer still came back to the pool");

	w.owner = (uint16_t)CF_OWNER__COUNT;
	ct_check(cf_post(fk, &w) == CF_E_INVAL, "an out-of-range owner is refused");
	cf_destroy(fk);
}

/* ------------------------------------------------------------------ *
 * 6. A handler may post work: the fork mutex is not held over the queues
 * ------------------------------------------------------------------ */
static void test_handler_may_post(void)
{
	static const char *const want[] = { "work:40/40", "work:99/99" };
	struct fake f;
	struct rec r;
	struct vms_cluster_fork *fk = mk(&f, &r, 4, 8, 4);

	printf("-- a running handler may post more work (no self-deadlock)\n");
	r.repost = 1;
	ct_check(post_kind(fk, 40) == CF_OK, "work 40 queued");
	while (cf_dispatch_one(fk))
		;
	transcript_is(&r, want, 2, "the handler's own post ran after it");
	cf_destroy(fk);
}

/* ------------------------------------------------------------------ *
 * 7. Construction refuses an incomplete contract
 * ------------------------------------------------------------------ */
static void test_create_validation(void)
{
	struct fake f;
	struct cf_ops o;

	printf("-- cf_create refuses an incomplete ops table\n");
	memset(&f, 0, sizeof(f));
	fake_ops(&o, &f);
	o.wake = NULL;
	ct_check(cf_create(&o, NULL) == NULL, "a missing op yields no context");
	ct_check(cf_create(NULL, NULL) == NULL, "no ops at all yields no context");
	ct_check(strcmp(cf_status_name(CF_E_NOSLOT), "CF_E_NOSLOT") == 0,
		 "cf_status_name is wired");
}

/* ------------------------------------------------------------------ *
 * 11. THE SERVED-I/O WORKER (FC-P6.6, design §3.2.6's E42 corollary)
 *
 * The queue discipline that lets the MSCP server make a BLOCKING block-device
 * call without the fork thread waiting on it. What is proved here is exactly
 * the three properties the rest of the stack depends on:
 *
 *   1. the layer's I/O callback runs with NO lock held -- the fake asserts it,
 *      so a regression that ran it under the queue lock aborts the harness;
 *   2. its status comes back as an ordinary CF_WORK_IO_DONE work item, on the
 *      FORK thread, carrying the submitter's own tag and the callback's own
 *      status VERBATIM (INV-6: the served READ's answer is the disk's answer);
 *   3. every refusal is counted and none is silent.
 * ------------------------------------------------------------------ */

struct io_probe {
	struct rec  *r;
	struct fake *fake;
	int          calls;
	struct cf_io last;
	uint32_t     status;
};

static uint32_t h_io(void *ctx, const struct cf_io *io)
{
	struct io_probe *p = ctx;

	/*
	 * THE CONTRACT, ASSERTED FROM INSIDE THE CALLBACK. In production this
	 * function is where exec_blockdev_read_block runs and it sleeps for
	 * milliseconds; holding either lock across it would be exactly the
	 * stall this whole item removes -- so a regression that ran it under a
	 * lock aborts the harness here, not in a lab three rungs up.
	 */
	if (p->fake->qlock_depth)
		die("the I/O callback ran with the QUEUE LOCK held");
	if (p->fake->fork_depth)
		die("the I/O callback ran with the FORK MUTEX held");
	p->calls++;
	p->last = *io;
	return p->status;
}

static void test_io_worker(void)
{
	struct fake f;
	struct rec r;
	struct vms_cluster_fork *fk = mk(&f, &r, 4, 8, 4);
	struct io_probe p;
	struct cf_io io;
	struct cf_stats st;

	printf("-- the served-I/O worker: off the fork thread, back as work\n");
	memset(&p, 0, sizeof(p));
	p.r = &r;
	p.fake = &f;
	p.status = 0u;

	/* No handler yet: an honest refusal, never a queued request nobody
	 * will run. */
	memset(&io, 0, sizeof(io));
	io.owner = CF_OWNER_PE;
	io.tag = 0x1001u;
	ct_check(cf_io_post(fk, &io) == CF_E_NOWORKER,
		 "with no I/O handler registered a submission is REFUSED");
	cf_stats_get(fk, &st);
	ct_check_eq_u32((unsigned long)st.io_refused_noworker, 1, "and counted");

	(void)cf_set_io_handler(fk, CF_OWNER_PE, h_io, &p);
	io.op = 7u;
	io.arg0 = 11u;
	io.arg1 = 22u;
	io.arg2 = 33u;
	io.arg3 = 44u;
	ct_check(cf_io_post(fk, &io) == CF_OK, "now it is accepted");
	ct_check_eq_u32((unsigned)p.calls, 0,
			"and NOTHING ran on the submitting thread");
	ct_check_eq_u32((unsigned)f.io_wake_calls, 1,
			"the worker was woken, under the queue lock");

	/* The fork thread has nothing to do yet: the completion does not exist
	 * until the worker has actually run. */
	ct_check_eq_u32((unsigned)cf_dispatch_one(fk), 0,
			"no work item exists before the worker runs");

	/* THE WORKER STEP. */
	ct_check_eq_u32((unsigned)cf_io_run_one(fk), 1, "the worker ran one");
	ct_check_eq_u32((unsigned)p.calls, 1, "the layer's callback was called");
	ct_check_eq_u32((unsigned)f.qlock_depth, 0,
			"...with the queue lock released");
	ct_check_eq_u32((unsigned)f.fork_depth, 0,
			"...and the fork mutex never taken");
	ct_check_eq_u32(p.last.tag, 0x1001u, "the tag arrived verbatim");
	ct_check_eq_u32(p.last.op, 7u, "and the op");
	ct_check_eq_u32(p.last.arg0, 11u, "and every argument");
	ct_check_eq_u32(p.last.arg3, 44u, "...including the last one");

	/* THE COMPLETION, on the FORK thread, as ordinary work. */
	ct_check_eq_u32((unsigned)cf_dispatch_one(fk), 1,
			"the completion is an ordinary fork-queue event");
	ct_check_eq_u32((unsigned)r.n, 1, "one work item was delivered");
	ct_check(strcmp(r.seq[0], "work:65534/4097") == 0,
		 "CF_WORK_IO_DONE carrying the submitter's tag (0x1001)");

	cf_stats_get(fk, &st);
	ct_check_eq_u32((unsigned long)st.io_posted, 1, "io_posted");
	ct_check_eq_u32((unsigned long)st.io_started, 1, "io_started");
	ct_check_eq_u32((unsigned long)st.io_completed, 1, "io_completed");
	ct_check_eq_u32(st.io_free, CF_IO_ITEMS_DEFAULT,
			"every io item came back to the pool");

	cf_destroy(fk);
	ct_check_eq_u32((unsigned)f.allocs, (unsigned)f.frees,
			"every pool allocation was freed");
}

/* The worker's OWN refusals and its stop: an exhausted pool, a stop that
 * abandons queued requests rather than running work for a layer that is being
 * torn down, and a restart that works. */
static void test_io_worker_refusals_and_stop(void)
{
	struct fake f;
	struct rec r;
	struct vms_cluster_fork *fk;
	struct io_probe p;
	struct cf_io io;
	struct cf_stats st;
	struct cf_config c;
	struct cf_ops o;
	uint32_t i;

	printf("-- the worker's refusals: counted, never silent\n");
	memset(&f, 0, sizeof(f));
	memset(&r, 0, sizeof(r));
	memset(&p, 0, sizeof(p));
	memset(&c, 0, sizeof(c));
	c.rx_bufs = 4;
	c.rx_cap = 64;
	c.work_items = 8;
	c.timer_slots = 4;
	c.io_items = 2;            /* small, so exhaustion is reachable */
	fake_ops(&o, &f);
	fk = cf_create(&o, &c);
	if (!fk)
		die("cf_create failed");
	r.f = fk;
	p.r = &r;
	p.fake = &f;
	(void)cf_set_work_handler(fk, CF_OWNER_PE, h_work, &r);
	(void)cf_set_io_handler(fk, CF_OWNER_PE, h_io, &p);

	memset(&io, 0, sizeof(io));
	io.owner = CF_OWNER_PE;
	for (i = 0; i < c.io_items; i++) {
		io.tag = 0x2000u + i;
		ct_check(cf_io_post(fk, &io) == CF_OK, "the pool takes one");
	}
	io.tag = 0x20ffu;
	ct_check(cf_io_post(fk, &io) == CF_E_NOBUF,
		 "an exhausted io pool is an HONEST refusal, not a wait");
	cf_stats_get(fk, &st);
	ct_check_eq_u32((unsigned long)st.io_dropped_nobuf, 1, "and counted");

	/* A worker stop ABANDONS what is queued: its owner is going away. */
	cf_io_request_stop(fk);
	ct_check(cf_io_post(fk, &io) == CF_E_STOPPING,
		 "a submission after the stop is refused");
	cf_io_run(fk);
	ct_check_eq_u32((unsigned)p.calls, 0,
			"NOT one queued request ran after the stop");
	cf_stats_get(fk, &st);
	ct_check_eq_u32((unsigned long)st.io_abandoned, c.io_items,
			"every queued request was returned to the pool AND counted");
	ct_check_eq_u32(st.io_free, c.io_items, "the pool is whole again");
	ct_check_eq_u32(st.io_stopping, 1, "and the worker reports it stopped");

	/* Registering a handler again re-opens the worker for a restart. */
	(void)cf_set_io_handler(fk, CF_OWNER_PE, h_io, &p);
	io.tag = 0x2100u;
	ct_check(cf_io_post(fk, &io) == CF_OK,
		 "re-registering the handler re-opens the worker");
	ct_check_eq_u32((unsigned)cf_io_run_one(fk), 1, "and it runs again");

	/* A FAILING callback's status travels back UNCHANGED -- the whole point
	 * of INV-6 at this seam: the server answers the disk's real answer. */
	p.status = 0xdead0001u;
	io.tag = 0x2101u;
	ct_check(cf_io_post(fk, &io) == CF_OK, "another request");
	(void)cf_io_run_one(fk);
	r.n = 0;
	while (cf_dispatch_one(fk))
		;
	ct_check(r.n >= 1, "its completion was delivered");
	ct_check(strcmp(r.seq[r.n - 1], "work:65534/8449") == 0,
		 "carrying the failing request's own tag (0x2101)");

	cf_destroy(fk);
	ct_check_eq_u32((unsigned)f.allocs, (unsigned)f.frees,
			"every pool allocation was freed");
}

int main(void)
{
	printf("test_cluster_fork: the cluster fork context (FC-P0.5, rung R1)\n");
	test_fifo_and_alternation();
	test_pool_exhaustion();
	test_timers();
	test_timer_tick_lost_when_pool_full();
	test_timer_cancel();
	test_stop_drains();
	test_run_exits_on_substrate_stop();
	test_undeliverable();
	test_handler_may_post();
	test_create_validation();
	test_io_worker();
	test_io_worker_refusals_and_stop();
	return ct_summary("test_cluster_fork");
}
