/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cluster_fork_threads.c - rung R1, concurrency half (FC-P0.5).
 *
 * test_cluster_fork.c drives the module deterministically with no thread; this
 * one runs the REAL shape: one consumer thread inside cf_run(), several
 * producer threads posting work, one thread delivering frames the way a receive
 * callback does, and one thread firing timer callbacks the way a substrate
 * timer does -- all against the same shipping vms_cluster_fork.c, with the ops
 * bound to pthread mutexes and a condition variable (the host stand-in for
 * exec_lock_t / exec_cv_t / exec_mutex_t, whose contracts are the same shape).
 *
 * WHAT IT PROVES, and how a failure shows up:
 *
 *   NO LOST WAKEUP  Producers deliberately pause so the consumer reaches the
 *                   cv wait, then post again. A wakeup lost anywhere in
 *                   cf_rx_deliver / cf_enqueue_locked / cf_request_stop leaves
 *                   the consumer asleep with work queued, the final join never
 *                   returns, and the watchdog alarm fails the test loudly
 *                   instead of hanging CI.
 *   NOTHING LOST    Every ACCEPTED post and frame is dispatched exactly once:
 *                   the totals the producers counted must equal the totals the
 *                   handlers counted and the module's own counters.
 *   FIFO PER SOURCE Each producer stamps a strictly increasing sequence; the
 *                   handler fails if it ever sees one go backwards, which is
 *                   what a queue that is not FIFO would produce.
 *   SERIALISED      The handlers assert that no two of them are ever inside the
 *                   fork mutex at once -- the one property the whole module
 *                   exists for.
 *   CLEAN STOP      After the stop, cf_run drains what was queued and returns.
 */

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vms_cluster_fork.h"
#include "cluster_test.h"

#define PRODUCERS   4
#define PER_PRODUCER 500
#define RX_FRAMES   800
#define TIMER_FIRES 300

/* ------------------------------------------------------------------ *
 * The pthread-backed ops
 * ------------------------------------------------------------------ */

struct hostops {
	pthread_mutex_t qlock;
	pthread_cond_t  qcv;
	pthread_mutex_t forkmtx;
	int             should_stop;
	int             in_fork;      /* handlers assert this never exceeds 1 */
};

static void ho_lock(void *c)   { pthread_mutex_lock(&((struct hostops *)c)->qlock); }
static void ho_unlock(void *c) { pthread_mutex_unlock(&((struct hostops *)c)->qlock); }

static int ho_wait(void *c)
{
	struct hostops *h = c;

	pthread_cond_wait(&h->qcv, &h->qlock);   /* drops + retakes qlock */
	return 0;
}

static void ho_wake(void *c) { pthread_cond_broadcast(&((struct hostops *)c)->qcv); }

static void ho_fork_lock(void *c)   { pthread_mutex_lock(&((struct hostops *)c)->forkmtx); }
static void ho_fork_unlock(void *c) { pthread_mutex_unlock(&((struct hostops *)c)->forkmtx); }

/* The module arms under the queue lock (must not sleep) and cancels outside it
 * (may sleep). Nothing to do here: the "substrate timer" is the timer thread. */
static void ho_timer_arm(void *c, uint32_t slot, uint32_t ms)
{
	(void)c; (void)slot; (void)ms;
}

static void ho_timer_cancel(void *c, uint32_t slot) { (void)c; (void)slot; }

static int ho_should_stop(void *c) { return ((struct hostops *)c)->should_stop; }

static void *ho_alloc(void *c, uint32_t n) { (void)c; return calloc(1, n ? n : 1); }
static void  ho_free(void *c, void *p)     { (void)c; free(p); }

/* ------------------------------------------------------------------ *
 * Shared test state
 * ------------------------------------------------------------------ */

struct shared {
	struct vms_cluster_fork *f;
	struct hostops *h;

	/* what the producers managed to hand over */
	unsigned accepted_work[PRODUCERS];
	unsigned accepted_rx;

	/* what the handlers saw */
	unsigned seen_work[PRODUCERS];
	unsigned last_seq[PRODUCERS];
	unsigned seen_rx;
	unsigned seen_timer;
	unsigned rx_last_seq;

	int order_violations;
	int overlap_violations;
	int rx_content_violations;
};

static struct shared S;

static void enter_fork(void)
{
	if (__atomic_add_fetch(&S.h->in_fork, 1, __ATOMIC_SEQ_CST) != 1)
		S.overlap_violations++;
}

static void leave_fork(void)
{
	__atomic_sub_fetch(&S.h->in_fork, 1, __ATOMIC_SEQ_CST);
}

static void h_work(void *ctx, const struct cf_work *w)
{
	(void)ctx;
	enter_fork();

	if (w->kind == CF_WORK_TIMER) {
		S.seen_timer++;
		/* The HELLO-cadence shape: the expiry handler re-arms, in the
		 * fork context, which is the only place a re-arm is legal. */
		(void)cf_timer_arm(S.f, CF_OWNER_PE, w->arg0, w->arg1, 10);
	} else {
		unsigned p = w->kind;

		if (p >= PRODUCERS) {
			S.order_violations++;
		} else {
			if (S.seen_work[p] && w->arg0 <= S.last_seq[p])
				S.order_violations++;   /* not FIFO */
			S.last_seq[p] = w->arg0;
			S.seen_work[p]++;
		}
	}
	leave_fork();
}

static void h_rx(void *ctx, const uint8_t *frame, uint32_t len)
{
	unsigned seq;

	(void)ctx;
	enter_fork();
	if (len != 16 || frame[0] != 0xab) {
		S.rx_content_violations++;
	} else {
		seq = (unsigned)frame[4] | ((unsigned)frame[5] << 8);
		if (S.seen_rx && seq <= S.rx_last_seq)
			S.order_violations++;
		S.rx_last_seq = seq;
	}
	S.seen_rx++;
	leave_fork();
}

/* ------------------------------------------------------------------ *
 * The threads
 * ------------------------------------------------------------------ */

static void *consumer(void *arg)
{
	(void)arg;
	cf_run(S.f);       /* returns only after a stop AND a full drain */
	return NULL;
}

static void *producer(void *arg)
{
	unsigned id = (unsigned)(long)arg;
	unsigned i;

	for (i = 0; i < PER_PRODUCER; i++) {
		struct cf_work w;

		memset(&w, 0, sizeof(w));
		w.owner = CF_OWNER_PE;
		w.kind  = (uint16_t)id;
		w.arg0  = i + 1;
		while (cf_post(S.f, &w) == CF_E_NOBUF)
			sched_yield();            /* backpressure, not a drop */
		S.accepted_work[id]++;
		if ((i % 50) == 0)
			usleep(300);              /* let the consumer go to sleep */
	}
	return NULL;
}

static void *rx_source(void *arg)
{
	uint8_t frame[16];
	unsigned i;

	(void)arg;
	for (i = 0; i < RX_FRAMES; i++) {
		memset(frame, 0, sizeof(frame));
		frame[0] = 0xab;
		frame[4] = (uint8_t)((i + 1) & 0xff);
		frame[5] = (uint8_t)(((i + 1) >> 8) & 0xff);
		while (cf_rx_deliver(S.f, frame, sizeof(frame)) == CF_E_NOBUF)
			sched_yield();
		S.accepted_rx++;
		if ((i % 40) == 0)
			usleep(200);
	}
	return NULL;
}

static void *timer_source(void *arg)
{
	unsigned i;

	(void)arg;
	for (i = 0; i < TIMER_FIRES; i++) {
		cf_timer_expired(S.f, 0);   /* exactly what a callback may do */
		usleep(50);
	}
	return NULL;
}

static void watchdog(int sig)
{
	(void)sig;
	/* async-signal-safe: write(2) and _exit(2) only */
	const char msg[] =
		"  FAIL the fork context stalled: a wakeup was lost "
		"(watchdog fired)\n";

	if (write(2, msg, sizeof(msg) - 1) < 0)
		_exit(2);
	_exit(1);
}

int main(void)
{
	struct hostops h;
	struct cf_ops o;
	struct cf_config c;
	pthread_t tc, tp[PRODUCERS], trx, ttm;
	struct cf_stats st;
	unsigned total_accepted = 0, total_seen = 0;
	long i;

	printf("test_cluster_fork_threads: the fork context under real threads\n");

	signal(SIGALRM, watchdog);
	alarm(60);

	memset(&h, 0, sizeof(h));
	pthread_mutex_init(&h.qlock, NULL);
	pthread_cond_init(&h.qcv, NULL);
	pthread_mutex_init(&h.forkmtx, NULL);
	memset(&S, 0, sizeof(S));
	S.h = &h;

	memset(&o, 0, sizeof(o));
	o.lock = ho_lock;             o.unlock = ho_unlock;
	o.wait = ho_wait;             o.wake = ho_wake;
	o.fork_lock = ho_fork_lock;   o.fork_unlock = ho_fork_unlock;
	o.timer_arm = ho_timer_arm;   o.timer_cancel = ho_timer_cancel;
	o.should_stop = ho_should_stop;
	o.alloc = ho_alloc;           o.free = ho_free;
	o.ctx = &h;

	memset(&c, 0, sizeof(c));
	c.rx_bufs = 8;         /* small on purpose: the pools must recycle */
	c.rx_cap = 64;
	c.work_items = 16;
	c.timer_slots = 4;

	S.f = cf_create(&o, &c);
	if (!S.f) {
		printf("  FAIL cf_create\n");
		return 1;
	}
	cf_set_rx_handler(S.f, h_rx, NULL);
	(void)cf_set_work_handler(S.f, CF_OWNER_PE, h_work, NULL);

	/* Arm slot 0 before the thread starts: process context, legal. Fire it
	 * once here too, with the pools still empty, so at least one expiry is
	 * GUARANTEED to be queued -- the timer thread below races the handler's
	 * re-arm on purpose, and how many of its fires land is not (and must not
	 * be) deterministic. What IS deterministic is that every one of them is
	 * accounted for, which is what this test asserts at the end. */
	ct_check(cf_timer_arm(S.f, CF_OWNER_PE, 1, 1, 10) == CF_OK,
		 "the cadence timer is armed before the fork thread starts");
	cf_timer_expired(S.f, 0);

	pthread_create(&tc, NULL, consumer, NULL);
	for (i = 0; i < PRODUCERS; i++)
		pthread_create(&tp[i], NULL, producer, (void *)i);
	pthread_create(&trx, NULL, rx_source, NULL);
	pthread_create(&ttm, NULL, timer_source, NULL);

	for (i = 0; i < PRODUCERS; i++)
		pthread_join(tp[i], NULL);
	pthread_join(trx, NULL);
	pthread_join(ttm, NULL);

	/* Every producer is finished, so nothing legitimate is refused by the
	 * stop; what is still queued must still run. */
	cf_request_stop(S.f);
	pthread_join(tc, NULL);
	alarm(0);

	cf_stats_get(S.f, &st);

	for (i = 0; i < PRODUCERS; i++) {
		total_accepted += S.accepted_work[i];
		total_seen += S.seen_work[i];
	}
	ct_check_eq_u32(total_seen, total_accepted,
			"every accepted work item was dispatched exactly once");
	ct_check_eq_u32(S.seen_rx, S.accepted_rx,
			"every accepted frame was dispatched exactly once");
	ct_check_eq_u32((unsigned long)st.rx_dispatched, S.accepted_rx,
			"the module's own rx counter agrees");
	ct_check_eq_u32((unsigned long)st.work_dispatched,
			total_seen + S.seen_timer,
			"the module's own work counter agrees");
	ct_check_eq_u32((unsigned)S.order_violations, 0,
			"no queue ever delivered out of order");
	ct_check_eq_u32((unsigned)S.overlap_violations, 0,
			"no two handlers ever ran at the same time");
	ct_check_eq_u32((unsigned)S.rx_content_violations, 0,
			"every frame arrived with the bytes that were sent");
	ct_check(st.waits > 0,
		 "the consumer really slept and was really woken (waits > 0)");
	ct_check_eq_u32(S.seen_timer, (unsigned long)st.timer_expiries,
			"every queued timer expiry ran in the fork context");
	ct_check(st.timer_expiries >= 1,
		 "at least the pre-armed cadence tick was posted and dispatched");
	ct_check_eq_u32((unsigned long)(st.timer_expiries + st.timer_coalesced +
					st.timer_stale + st.timer_dropped_nobuf),
			TIMER_FIRES + 1,
			"every substrate timer callback is accounted for exactly once");
	ct_check_eq_u32(st.rx_free, c.rx_bufs,
			"every receive buffer is back in the pool");
	ct_check_eq_u32(st.work_free, c.work_items,
			"every work item is back in the pool");
	printf("  info %llu work dispatches, %llu rx, %llu timer expiries, %llu sleeps\n",
	       (unsigned long long)st.work_dispatched,
	       (unsigned long long)st.rx_dispatched,
	       (unsigned long long)st.timer_expiries,
	       (unsigned long long)st.waits);

	cf_destroy(S.f);
	pthread_mutex_destroy(&h.qlock);
	pthread_cond_destroy(&h.qcv);
	pthread_mutex_destroy(&h.forkmtx);
	return ct_summary("test_cluster_fork_threads");
}
