// SPDX-License-Identifier: GPL-2.0
/*
 * vms_cluster_fork_bind.c - the executive glue of the cluster fork context
 * (FC-P0.5): it binds `struct cf_ops` to the substrate seam and owns every
 * substrate object the fork context needs, BY VALUE.
 *
 * Read vms_cluster_fork.h first. The division of labour is design §3.9's
 * pure/glue split applied to the fork module:
 *
 *   vms_cluster_fork.c   the queues, the dispatch, the timer bookkeeping --
 *                        pure, host-testable (rung R1), no substrate type.
 *   THIS FILE            exec_rxlock_t / exec_cv_t / exec_mutex_t /
 *                        exec_kthread_t / exec_timer_t, the thread entry, the
 *                        timer callbacks, and start/stop. It is the ONLY file
 *                        in the cluster stack that names §15 or §16 (design
 *                        §3.2.2's leak table), which is what keeps kthread and
 *                        timer idioms out of every layer above.
 *
 * THE FOUR SUBSTRATE OBJECTS AND WHY EACH IS THE TYPE IT IS
 *
 *   qlock   (§1b exec_rxlock_t) the receive-level queue lock of CONTRACT RULE
 *                              14.1 (design §3.2.3 RULING, FC-P0.16). Legal
 *                              from ANY context -- receive, timer, process, or
 *                              the fork thread -- held for a bounded copy and
 *                              a few pointer moves, a LEAF in the lock order.
 *                              qflags carries the Linux irqsave word (unused
 *                              on NetBSD, where the IPL lives in the mutex);
 *                              exactly one holder exists at a time by
 *                              construction, so ONE scratch field in this
 *                              struct is enough to carry it across the
 *                              matching acquire/release (or acquire/wait/
 *                              release) pair.
 *   qcv     (§2 exec_cv_t)     paired with qlock, and ONLY with qlock -- a
 *                              wait/wake pair whose two sides use different
 *                              locks is outside the seam's contract and loses
 *                              wakeups. cf_run's waiter (via exec_cv_wait_rx,
 *                              §1b, thread context only) and every cf_post /
 *                              cf_rx_deliver waker (exec_cv_signal/broadcast,
 *                              legal from receive level under the rxlock)
 *                              share it.
 *   forkmtx (§7 exec_mutex_t)  `vms_cluster_fork_mutex' itself: VMS's fork-IPL
 *                              serialisation. SLEEPABLE, because a dispatched
 *                              handler may allocate, may cancel a timer and may
 *                              take the lock manager's locks -- design §3.3's
 *                              "the fork thread never sleeps holding an
 *                              exec_lock_t" is exactly why this is not a
 *                              spinlock.
 *   thread  (§15 exec_kthread) the one "CNXMAN fork" per node.
 *
 * WHY THE STOP PATH SETS ITS OWN FLAG FIRST. Neither substrate's kthread-stop
 * wakes a thread that is asleep on a condition variable in a way that makes the
 * sleeper's PREDICATE true: Linux's kthread_stop makes the task runnable and
 * the wait loop simply re-tests its condition and sleeps again; NetBSD's join
 * waits for an exit that would never come. So vms_cluster_fork_stop() calls
 * cf_request_stop() -- which mutates the predicate and broadcasts under qlock,
 * the seam's lost-wakeup-free idiom -- and only THEN joins.
 *
 * INCLUDES: exec_kbackend.h plus kernel-core headers, nothing else
 * (tools/ci/cluster_core_includes_gate.sh).
 */

#include "vms_internal.h"    /* the SS$_ vocabulary + the host's fixed-width types */
#include "exec_kbackend.h"   /* §1b §2 §4 §7 §15 §16: this file's whole world */
#include "vms_cluster.h"
#include "vms_cluster_fork.h"

/*
 * ONE BUFFER, TWO SPELLINGS, ASSERTED. struct cf_lanbuf is the pure module's
 * name for the core-owned receive buffer; exec_lanbuf_t is the seam's. They
 * must be one layout, or the FC-P0.9 receive path would be copying between two
 * structs that only look alike -- the same single-lineage discipline vms_pe.c
 * applies to EXEC_SS_NOSUCHDEV vs SS$_NOSUCHDEV. __builtin_offsetof, not
 * offsetof: it needs no header on either substrate.
 */
_Static_assert(sizeof(struct cf_lanbuf) == sizeof(exec_lanbuf_t),
	       "cf_lanbuf must be exec_lanbuf_t");
_Static_assert(__builtin_offsetof(struct cf_lanbuf, data) ==
	       __builtin_offsetof(exec_lanbuf_t, data),
	       "cf_lanbuf.data must sit where exec_lanbuf_t.data sits");
_Static_assert(__builtin_offsetof(struct cf_lanbuf, cap) ==
	       __builtin_offsetof(exec_lanbuf_t, cap),
	       "cf_lanbuf.cap must sit where exec_lanbuf_t.cap sits");
_Static_assert(__builtin_offsetof(struct cf_lanbuf, len) ==
	       __builtin_offsetof(exec_lanbuf_t, len),
	       "cf_lanbuf.len must sit where exec_lanbuf_t.len sits");

/* ==================================================================== *
 * 1. The binding's own state
 * ==================================================================== */

struct cf_bind;

/*
 * One armed timer. exec_timer_init takes a {cb, ctx} pair and the callback gets
 * only that ctx, so each slot carries its own back-pointer + index: that is how
 * a substrate callback knows WHICH slot fired without a global.
 */
struct cf_timer_bind {
	struct cf_bind *b;
	uint32_t        idx;
	exec_timer_t    t;
};

struct cf_bind {
	exec_rxlock_t  qlock;
	exec_rxflags_t qflags;  /* scratch for the one live holder (§1b) */
	exec_cv_t      qcv;
	exec_cv_t      iocv;    /* the SERVED-I/O WORKER's own (FC-P6.6) */
	exec_mutex_t   forkmtx;
	exec_kthread_t thread;
	exec_kthread_t ioworker;

	struct vms_cluster_fork *fork;
	struct cf_timer_bind    *timers;
	uint32_t                 n_timers;
	int                      thread_started;
	int                      ioworker_started;
};

/* The binding hangs off the fork context's own ops.ctx, so `struct vms_cluster'
 * needs no extra field and the frozen FC-P0.1 header stays frozen. */
static struct cf_bind *bind_of(struct vms_cluster *cl)
{
	if (!cl || !cl->fork)
		return NULL;
	return (struct cf_bind *)cf_ops_ctx(cl->fork);
}

/* ==================================================================== *
 * 2. The ops -- one line each, straight onto the seam
 * ==================================================================== */

/*
 * §1b: legal from ANY context (receive, timer, process, or the fork thread).
 * qflags is safe as a single scratch field because the rxlock itself
 * guarantees exactly one live holder at a time -- the same reasoning
 * spin_lock_irqsave's caller-supplied flags word relies on, just stored in
 * the binding instead of a caller's stack local so cfb_wait can reach it too.
 */
static void cfb_lock(void *ctx)
{
	struct cf_bind *b = (struct cf_bind *)ctx;

	exec_rxlock_acquire(&b->qlock, &b->qflags);
}

static void cfb_unlock(void *ctx)
{
	struct cf_bind *b = (struct cf_bind *)ctx;

	exec_rxlock_release(&b->qlock, &b->qflags);
}

/* THREAD CONTEXT ONLY (§1b): cf_wait_ready, the sole caller of ops->wait, runs
 * only inside cf_run -- the fork thread. */
static int cfb_wait(void *ctx)
{
	struct cf_bind *b = (struct cf_bind *)ctx;

	return exec_cv_wait_rx(&b->qcv, &b->qlock, &b->qflags);
}

/* Broadcast, not signal: the fork thread is the only sleeper today, but a
 * snapshot reader or a future drain-waiter on the same cv must not be left
 * asleep because a single wake went to the wrong one. */
static void cfb_wake(void *ctx)
{
	exec_cv_broadcast(&((struct cf_bind *)ctx)->qcv);
}

static void cfb_fork_lock(void *ctx)
{
	exec_mutex_lock(&((struct cf_bind *)ctx)->forkmtx);
}

static void cfb_fork_unlock(void *ctx)
{
	exec_mutex_unlock(&((struct cf_bind *)ctx)->forkmtx);
}

/* Called with the queue lock held: exec_timer_arm must not sleep, and
 * re-arming an armed timer MOVES it (mod_timer / callout_schedule). */
static void cfb_timer_arm(void *ctx, uint32_t slot, uint32_t ms)
{
	struct cf_bind *b = (struct cf_bind *)ctx;

	if (slot < b->n_timers)
		exec_timer_arm(&b->timers[slot].t, ms);
}

/* Called with NO lock held: this one WAITS OUT a callback already running. */
static void cfb_timer_cancel(void *ctx, uint32_t slot)
{
	struct cf_bind *b = (struct cf_bind *)ctx;

	if (slot < b->n_timers)
		exec_timer_cancel(&b->timers[slot].t);
}

static int cfb_should_stop(void *ctx)
{
	return exec_kthread_should_stop(&((struct cf_bind *)ctx)->thread);
}

/*
 * The SERVED-I/O WORKER's three ops (FC-P6.6). Identical in shape to the fork
 * thread's, and deliberately bound to a SECOND exec_cv_t on the SAME rxlock:
 * one interlock (CONTRACT RULE 14.1 says exactly ONE exec_rxlock_t is shared
 * between receive and thread context, and this keeps that true), two sleep
 * queues, so a received frame's wake does not touch the worker and the worker's
 * completion does not touch the fork thread's sleep beyond the cf_post it
 * already makes.
 */
static int cfb_io_wait(void *ctx)
{
	struct cf_bind *b = (struct cf_bind *)ctx;

	return exec_cv_wait_rx(&b->iocv, &b->qlock, &b->qflags);
}

static void cfb_io_wake(void *ctx)
{
	exec_cv_broadcast(&((struct cf_bind *)ctx)->iocv);
}

static int cfb_io_should_stop(void *ctx)
{
	struct cf_bind *b = (struct cf_bind *)ctx;

	/* Before the worker thread exists there is nothing to stop, and asking
	 * an uninitialised handle would be a read of a field no one wrote. */
	return b->ioworker_started
		       ? exec_kthread_should_stop(&b->ioworker) : 0;
}

static void *cfb_alloc(void *ctx, uint32_t n)
{
	(void)ctx;
	return exec_zalloc((size_t)n);
}

static void cfb_free(void *ctx, void *p)
{
	(void)ctx;
	exec_free(p);
}

static const struct cf_ops cf_exec_ops = {
	cfb_lock, cfb_unlock, cfb_wait, cfb_wake,
	cfb_fork_lock, cfb_fork_unlock,
	cfb_timer_arm, cfb_timer_cancel,
	cfb_should_stop,
	cfb_io_wait, cfb_io_wake, cfb_io_should_stop,
	cfb_alloc, cfb_free,
	(void *)0
};

/* ==================================================================== *
 * 3. The two substrate callbacks
 * ==================================================================== */

/* CONTRACT RULE 2, and the only place in the stack that runs in timer context:
 * post a work item and wake. The expiry's handler runs later, in the fork
 * thread, under the fork mutex, like every other event. */
static void cfb_timer_cb(void *ctx)
{
	struct cf_timer_bind *tb = (struct cf_timer_bind *)ctx;

	cf_timer_expired(tb->b->fork, tb->idx);
}

/* The one kernel thread per node. Its whole body is the pure drain loop; it
 * returns when a stop has been requested AND the queues are drained. */
static int cfb_thread(void *arg)
{
	struct cf_bind *b = (struct cf_bind *)arg;

	cf_run(b->fork);
	return 0;
}

/*
 * The SERVED-I/O WORKER thread (FC-P6.6). Its whole body is the pure run loop,
 * which calls the submitting layer's I/O callback with NO lock held -- the one
 * context in this stack where a blocking substrate call (exec_blockdev_*) is
 * legal. It returns when a worker stop has been requested.
 */
static int cfb_io_thread(void *arg)
{
	struct cf_bind *b = (struct cf_bind *)arg;

	cf_io_run(b->fork);
	return 0;
}

/* ==================================================================== *
 * 4. Construction and teardown
 * ==================================================================== */

static int cfb_timers_create(struct cf_bind *b, uint32_t n)
{
	uint32_t i;

	b->timers = (struct cf_timer_bind *)exec_zalloc(
			(size_t)n * sizeof(*b->timers));
	if (!b->timers)
		return 0;
	b->n_timers = n;
	for (i = 0; i < n; i++) {
		b->timers[i].b   = b;
		b->timers[i].idx = i;
		exec_timer_init(&b->timers[i].t, cfb_timer_cb, &b->timers[i]);
	}
	return 1;
}

/* Cancel (waiting out any running callback) then destroy every timer. Called
 * only after the fork thread has been joined, so nothing can re-arm behind us.
 * exec_timer_destroy is a no-op on Linux and MANDATORY on NetBSD. */
static void cfb_timers_destroy(struct cf_bind *b)
{
	uint32_t i;

	if (!b->timers)
		return;
	for (i = 0; i < b->n_timers; i++) {
		exec_timer_cancel(&b->timers[i].t);
		exec_timer_destroy(&b->timers[i].t);
	}
	exec_free(b->timers);
	b->timers = (struct cf_timer_bind *)0;
	b->n_timers = 0;
}

static void cfb_free_bind(struct cf_bind *b)
{
	cfb_timers_destroy(b);
	exec_mutex_destroy(&b->forkmtx);
	exec_cv_destroy(&b->iocv);
	exec_cv_destroy(&b->qcv);
	exec_rxlock_destroy(&b->qlock);
	exec_free(b);
}

int vms_cluster_fork_start(struct vms_cluster *cl, const struct cf_config *cfg)
{
	struct cf_config eff;
	struct cf_ops ops;
	struct cf_bind *b;
	int status;

	if (!cl)
		return SS__BADPARAM;
	if (cl->fork)
		return SS__NORMAL;   /* already running: idempotent */

	cf_config_normalize(&eff, cfg);

	b = (struct cf_bind *)exec_zalloc(sizeof(*b));
	if (!b)
		return SS__INSFMEM;
	exec_rxlock_init(&b->qlock);
	exec_cv_init(&b->qcv);
	exec_cv_init(&b->iocv);
	exec_mutex_init(&b->forkmtx);

	if (!cfb_timers_create(b, eff.timer_slots)) {
		cfb_free_bind(b);
		return SS__INSFMEM;
	}

	ops = cf_exec_ops;
	ops.ctx = b;
	b->fork = cf_create(&ops, &eff);
	if (!b->fork) {
		cfb_free_bind(b);
		return SS__INSFMEM;
	}

	/* Published BEFORE the thread starts: cf_run dereferences b->fork, and
	 * a poster reaching cl->fork can only find a fully built context. */
	cl->fork = b->fork;

	status = exec_kthread_create(&b->thread, cfb_thread, b, "cnxman_fork");
	if (status != 0) {
		/* Honest end of the road on a substrate with no §15 binding:
		 * no thread, no fork context, no pretence (Rule 9). */
		cl->fork = (struct vms_cluster_fork *)0;
		cf_destroy(b->fork);
		cfb_free_bind(b);
		return status;
	}
	b->thread_started = 1;
	return SS__NORMAL;
}

/*
 * The SERVED-I/O WORKER's lifecycle (FC-P6.6). Separate from the fork thread's
 * because serving disks is a ROLE (vms_cluster_fork.h §8): the MSCP server's own
 * start/stop own it, and a node that serves nothing never carries the thread.
 */
int vms_cluster_fork_worker_start(struct vms_cluster *cl)
{
	struct cf_bind *b = bind_of(cl);
	int status;

	if (!b)
		return SS__NOSUCHDEV;        /* no fork context: Rule 9 */
	if (b->ioworker_started)
		return SS__NORMAL;           /* already running: idempotent */

	/* Clear a stop left by a previous cycle BEFORE the thread exists, so it
	 * cannot start and immediately exit on a stale flag. */
	cf_io_start(b->fork);

	status = exec_kthread_create(&b->ioworker, cfb_io_thread, b,
				     "mscp_srv_io");
	if (status != 0)
		return status;   /* honest: no thread, and the caller is told */
	b->ioworker_started = 1;
	return SS__NORMAL;
}

void vms_cluster_fork_worker_stop(struct vms_cluster *cl)
{
	struct cf_bind *b = bind_of(cl);

	if (!b || !b->ioworker_started)
		return;

	/* The predicate mutation the kthread-stop itself cannot make (see this
	 * file's header): set the flag and broadcast on the worker's OWN cv,
	 * then join. After the join no callback is running, so the layer that
	 * lent the worker a buffer may free it. */
	cf_io_request_stop(b->fork);
	exec_kthread_stop(&b->ioworker);
	b->ioworker_started = 0;
}

void vms_cluster_fork_stop(struct vms_cluster *cl)
{
	struct cf_bind *b = bind_of(cl);

	if (!b)
		return;

	/* 0. The SERVED-I/O WORKER goes FIRST and is JOINED: it is the only
	 *    context that can still be inside a layer's buffer, and everything
	 *    below frees state that layer owns. Idempotent, so a layer that
	 *    already stopped its own worker pays nothing here. */
	vms_cluster_fork_worker_stop(cl);

	/* 1. Refuse new work and wake the sleeper -- the predicate mutation the
	 *    kthread-stop itself cannot make. Work already queued still runs. */
	cf_request_stop(b->fork);

	/* 2. Join. After this the fork thread is gone, so nothing can arm a
	 *    timer or touch a queue behind the teardown below. */
	if (b->thread_started) {
		exec_kthread_stop(&b->thread);
		b->thread_started = 0;
	}

	/* 3. Timers BEFORE the queues, never after: each cancel waits out a
	 *    callback already running, and that callback's only act is
	 *    cf_timer_expired() on b->fork. Freeing the fork context first would
	 *    leave an in-flight callback posting into freed memory. */
	cfb_timers_destroy(b);

	/* 4. Now nothing can reach the queues: unpublish and free. */
	cl->fork = (struct vms_cluster_fork *)0;
	cf_destroy(b->fork);
	b->fork = (struct vms_cluster_fork *)0;
	cfb_free_bind(b);
}

/* ==================================================================== *
 * 5. Snapshot serialisation for process context
 * ==================================================================== */

void vms_cluster_fork_enter(struct vms_cluster *cl)
{
	struct cf_bind *b = bind_of(cl);

	if (b)
		exec_mutex_lock(&b->forkmtx);
}

void vms_cluster_fork_leave(struct vms_cluster *cl)
{
	struct cf_bind *b = bind_of(cl);

	if (b)
		exec_mutex_unlock(&b->forkmtx);
}
