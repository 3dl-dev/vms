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

/*
 * The SERVED-I/O WORKER's leg (FC-P6.6). Each request's callback SLEEPS -- the
 * host stand-in for a served disk read -- and the whole point of the leg is
 * that the fork thread keeps dispatching protocol while it does.
 */
#define IO_REQUESTS 200
#define IO_SLEEP_US 200

/* ------------------------------------------------------------------ *
 * The pthread-backed ops
 * ------------------------------------------------------------------ */

struct hostops {
	pthread_mutex_t qlock;
	pthread_cond_t  qcv;
	pthread_cond_t  iocv;         /* the SERVED-I/O WORKER's own (FC-P6.6) */
	pthread_mutex_t forkmtx;
	int             should_stop;
	int             io_should_stop;
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

/*
 * The SERVED-I/O WORKER must never take the fork mutex (FC-P6.6): it is the one
 * thread that blocks, and a blocking thread holding fork-IPL serialisation is
 * the stall this item removes. Asserted by IDENTITY -- if this lock is ever
 * taken on the worker's own thread the test says so -- rather than by a shared
 * flag, which would only prove the fork thread was busy at the time.
 */
static pthread_t g_io_worker_tid;
static int       g_io_worker_tid_valid;
static int       g_io_fork_lock_violations;

static void ho_fork_lock(void *c)
{
	if (g_io_worker_tid_valid &&
	    pthread_equal(pthread_self(), g_io_worker_tid))
		g_io_fork_lock_violations++;
	pthread_mutex_lock(&((struct hostops *)c)->forkmtx);
}
static void ho_fork_unlock(void *c) { pthread_mutex_unlock(&((struct hostops *)c)->forkmtx); }

/* The module arms under the queue lock (must not sleep) and cancels outside it
 * (may sleep). Nothing to do here: the "substrate timer" is the timer thread. */
static void ho_timer_arm(void *c, uint32_t slot, uint32_t ms)
{
	(void)c; (void)slot; (void)ms;
}

static void ho_timer_cancel(void *c, uint32_t slot) { (void)c; (void)slot; }

static int ho_should_stop(void *c) { return ((struct hostops *)c)->should_stop; }

/*
 * The worker's three ops: a SECOND condition variable on the SAME queue lock,
 * exactly as vms_cluster_fork_bind.c binds them on both substrates.
 */
static int ho_io_wait(void *c)
{
	struct hostops *h = c;

	pthread_cond_wait(&h->iocv, &h->qlock);   /* drops + retakes qlock */
	return 0;
}

static void ho_io_wake(void *c) { pthread_cond_broadcast(&((struct hostops *)c)->iocv); }
static int  ho_io_should_stop(void *c) { return ((struct hostops *)c)->io_should_stop; }

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

	/* the SERVED-I/O WORKER leg (FC-P6.6) */
	unsigned accepted_io;          /* requests the worker took             */
	unsigned io_calls;             /* callbacks that really ran            */
	unsigned io_done_seen;         /* CF_WORK_IO_DONE items dispatched     */
	int      io_in_callback;       /* non-zero while a "disk" is busy      */
	unsigned fork_progress_during_io;  /* THE PROPERTY: fork-thread events
					    * dispatched while a disk was busy */
	int      io_tag_violations;    /* a completion's tag/status was altered*/
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

	if (__atomic_load_n(&S.io_in_callback, __ATOMIC_SEQ_CST))
		S.fork_progress_during_io++;

	if (w->kind == CF_WORK_IO_DONE) {
		/* The worker's answer, delivered here and nowhere else. The
		 * callback returned its own tag as the status, so a completion
		 * whose two halves disagree means the module rewrote one. */
		if (w->arg0 != w->arg1)
			S.io_tag_violations++;
		S.io_done_seen++;
	} else if (w->kind == CF_WORK_TIMER) {
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

/*
 * THE I/O CALLBACK: what runs on the worker thread. It sleeps, which is the
 * whole point -- on the shipping stack this is exec_blockdev_read_block on a
 * served volume. It must never be holding the fork mutex, and the fork thread
 * must keep running while it is here.
 */
static uint32_t h_io(void *ctx, const struct cf_io *io)
{
	(void)ctx;
	__atomic_add_fetch(&S.io_in_callback, 1, __ATOMIC_SEQ_CST);
	usleep(IO_SLEEP_US);
	S.io_calls++;
	__atomic_sub_fetch(&S.io_in_callback, 1, __ATOMIC_SEQ_CST);
	return io->tag;   /* echoed back as the status, so both can be checked */
}

static void *io_worker(void *arg)
{
	(void)arg;
	g_io_worker_tid = pthread_self();
	__atomic_store_n(&g_io_worker_tid_valid, 1, __ATOMIC_SEQ_CST);
	cf_io_run(S.f);
	return NULL;
}

static void *io_source(void *arg)
{
	unsigned i;

	(void)arg;
	for (i = 0; i < IO_REQUESTS; i++) {
		struct cf_io io;

		memset(&io, 0, sizeof(io));
		io.owner = CF_OWNER_PE;
		io.tag = i + 1u;
		while (cf_io_post(S.f, &io) == CF_E_NOBUF)
			sched_yield();          /* backpressure, not a drop */
		S.accepted_io++;
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
	pthread_t tc, tp[PRODUCERS], trx, ttm, tiow, tios;
	struct cf_stats st;
	unsigned total_accepted = 0, total_seen = 0;
	long i;

	printf("test_cluster_fork_threads: the fork context under real threads\n");

	signal(SIGALRM, watchdog);
	alarm(60);

	memset(&h, 0, sizeof(h));
	pthread_mutex_init(&h.qlock, NULL);
	pthread_cond_init(&h.qcv, NULL);
	pthread_cond_init(&h.iocv, NULL);
	pthread_mutex_init(&h.forkmtx, NULL);
	memset(&S, 0, sizeof(S));
	S.h = &h;

	memset(&o, 0, sizeof(o));
	o.lock = ho_lock;             o.unlock = ho_unlock;
	o.wait = ho_wait;             o.wake = ho_wake;
	o.fork_lock = ho_fork_lock;   o.fork_unlock = ho_fork_unlock;
	o.timer_arm = ho_timer_arm;   o.timer_cancel = ho_timer_cancel;
	o.should_stop = ho_should_stop;
	o.io_wait = ho_io_wait;       o.io_wake = ho_io_wake;
	o.io_should_stop = ho_io_should_stop;
	o.alloc = ho_alloc;           o.free = ho_free;
	o.ctx = &h;

	memset(&c, 0, sizeof(c));
	c.rx_bufs = 8;         /* small on purpose: the pools must recycle */
	c.rx_cap = 64;
	c.work_items = 16;
	c.timer_slots = 4;
	c.io_items = 4;        /* small on purpose: the io pool must recycle */

	S.f = cf_create(&o, &c);
	if (!S.f) {
		printf("  FAIL cf_create\n");
		return 1;
	}
	cf_set_rx_handler(S.f, h_rx, NULL);
	(void)cf_set_work_handler(S.f, CF_OWNER_PE, h_work, NULL);
	(void)cf_set_io_handler(S.f, CF_OWNER_PE, h_io, NULL);

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
	pthread_create(&tiow, NULL, io_worker, NULL);
	pthread_create(&tios, NULL, io_source, NULL);

	for (i = 0; i < PRODUCERS; i++)
		pthread_join(tp[i], NULL);
	pthread_join(trx, NULL);
	pthread_join(ttm, NULL);
	pthread_join(tios, NULL);

	/* Let every SUBMITTED I/O finish before any stop is asked for, so the
	 * accounting below is exact: a stop refuses new work, and a completion
	 * posted after it would be an honest drop rather than a lost request.
	 * The watchdog covers a real stall here. */
	for (;;) {
		cf_stats_get(S.f, &st);
		if (st.io_completed >= S.accepted_io)
			break;
		usleep(500);
	}
	cf_io_request_stop(S.f);
	h.io_should_stop = 1;
	pthread_join(tiow, NULL);

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
			total_seen + S.seen_timer + S.io_done_seen,
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

	/* ---- the SERVED-I/O WORKER leg (FC-P6.6) ---- */
	ct_check_eq_u32(S.io_calls, S.accepted_io,
			"every accepted I/O request really ran on the worker");
	ct_check_eq_u32(S.io_done_seen, S.accepted_io,
			"...and every one came back as a CF_WORK_IO_DONE on the "
			"FORK thread -- none lost, none doubled");
	ct_check_eq_u32((unsigned)S.io_tag_violations, 0,
			"each completion carried its OWN tag and its callback's "
			"OWN status, unaltered (INV-6)");
	ct_check_eq_u32((unsigned)g_io_fork_lock_violations, 0,
			"and the worker thread NEVER took the fork mutex");
	/*
	 * THE PROPERTY FC-P6.6 EXISTS FOR, measured rather than asserted: with
	 * IO_REQUESTS blocking "disk" calls of IO_SLEEP_US each, the fork thread
	 * went on dispatching frames, work and timer expiries THROUGHOUT. Before
	 * this item the same work would have run ON the fork thread and this
	 * count could only have been zero.
	 */
	ct_check(S.fork_progress_during_io > 0,
		 "the fork thread kept dispatching WHILE a served I/O was "
		 "blocking on the worker");
	ct_check_eq_u32(st.io_free, c.io_items,
			"every io item is back in the pool");
	printf("  info %u served I/Os, %u fork dispatches overlapped a busy disk\n",
	       S.io_calls, S.fork_progress_during_io);
	printf("  info %llu work dispatches, %llu rx, %llu timer expiries, %llu sleeps\n",
	       (unsigned long long)st.work_dispatched,
	       (unsigned long long)st.rx_dispatched,
	       (unsigned long long)st.timer_expiries,
	       (unsigned long long)st.waits);

	cf_destroy(S.f);
	pthread_mutex_destroy(&h.qlock);
	pthread_cond_destroy(&h.qcv);
	pthread_cond_destroy(&h.iocv);
	pthread_mutex_destroy(&h.forkmtx);
	return ct_summary("test_cluster_fork_threads");
}
