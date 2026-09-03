/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster_fork.h - THE CLUSTER FORK CONTEXT: the one execution context in
 * which every line of cluster protocol code runs (FC-P0.5).
 *
 * Design: docs/design-faithful-cluster-executive.md §3.3 (execution model --
 * one fork context, like VMS), §3.2.2 §15/§16 (the kthread and timer seam
 * families and their two CONTRACT RULES), §3.9 (pure logic + injected ops +
 * glue + host rung).
 *
 * WHY ONE CONTEXT, AND WHY IT IS FAITHFUL. VMS serialises PEDRIVER/SCS/CNXMAN
 * at fork IPL: one logical thread of control mutates the PDT/PB/CDT/CSB/RSB
 * databases. The published architecture describes the same shape from the other
 * side: the port driver, SYS$SCS and SYS$CLUSTER are LOADABLE EXECUTIVE
 * COMPONENTS, not processes (Davis, "VAXcluster Principles", 1993, pp. 2-55 /
 * 2-56, §2.14 "Block Diagram of the VMS Implementation of SCA" -- the only
 * *process* in that figure, the Configure Process, is explicitly named as NOT
 * part of SCA); and the Connection Manager is two modules (CNXMAN + CONMAN)
 * linked into ONE component that is "just one of several SYSAPs" (pp. 7-2/7-3,
 * §7.2). When that executive code cannot proceed it does NOT block a thread: it
 * queues a CDRP -- a continuation -- to the object it is waiting on and is
 * resumed later in the same context, and those wait queues are FIRST IN FIRST
 * OUT, "queue priority is based on time spent in the queue" (pp. 2-45/2-46,
 * §2.9 "VMS SCS Waits"; the same pages note the PDT pool-wait queue is
 * "examined once a second", i.e. driven by a timer, not by a thread that
 * slept). This module is OVMX's shape of exactly that: a work queue of
 * continuations plus a receive queue, drained one at a time by one context,
 * with timers that POST rather than run.
 *
 * Rejected (design §3.3): a kthread per layer with message passing -- more
 * concurrency than VMS has, and the barrier/rebuild coupling is precisely where
 * cross-database ordering matters.
 *
 * ------------------------------------------------------------------------
 * THE FOUR CONTEXTS THAT TOUCH THIS MODULE, AND WHAT EACH MAY DO
 * ------------------------------------------------------------------------
 *
 *  (1) RECEIVE context -- the substrate's rx callback (Linux softirq, NetBSD
 *      softint, or the VAX qe/xq driver's own interrupt). It calls exactly ONE
 *      entry point, cf_rx_deliver(), which copies the frame into a
 *      pre-allocated core-owned buffer, queues it and wakes the fork thread.
 *      No allocation, no sleep, no protocol -- exec_kbackend.h CONTRACT RULE 1.
 *
 *  (2) TIMER context -- a no-sleep substrate callback. It calls exactly ONE
 *      entry point, cf_timer_expired(), which posts a work item and wakes.
 *      The handler for the expiry runs later, in the fork thread, under the
 *      fork mutex, like every other event -- exec_kbackend.h CONTRACT RULE 2.
 *      No layer arms a substrate timer itself: they all go through
 *      cf_timer_arm()/cf_timer_cancel() here, so timer idioms never spread.
 *
 *  (3) PROCESS context -- an ioctl thread ($ENQ needing the wire, MOUNT needing
 *      an MSCP command, CLUSTER_START/STOP, a snapshot reader). It may
 *      cf_post() work, may take the fork mutex to read a snapshot
 *      (vms_cluster_fork_enter/leave), and must never touch layer state
 *      otherwise.
 *
 *  (4) The SERVED-I/O WORKER thread (§10 below, FC-P6.6). A SECOND
 *      exec_kthread whose entire job is to make the BLOCKING substrate calls
 *      the fork thread must never make -- today the MSCP disk server's
 *      exec_blockdev_read/write_block on a served unit. It runs one layer
 *      callback at a time, in process context, holding NO lock, and its result
 *      comes back to the layer as an ordinary work item on the fork queue. It
 *      runs no protocol and touches no protocol state.
 *
 * The FORK THREAD itself is the only context that runs protocol handlers, and
 * it runs them one at a time under the fork mutex.
 *
 * ------------------------------------------------------------------------
 * LOCK ORDER (design §3.3; never inverted)
 * ------------------------------------------------------------------------
 *
 *      fork mutex  ->  res->lock  ->  vms_lock_id_lock
 *      fork mutex  ->  queue lock (a LEAF: nothing is ever acquired under it)
 *
 * The SERVED-I/O WORKER (context 4) takes the queue lock and NOTHING ELSE, and
 * never while it is inside a layer's I/O callback: it pops one request under
 * the lock, DROPS the lock, runs the callback (which may sleep for tens of
 * milliseconds on a disk), then re-takes the lock to post the completion. It
 * never takes the fork mutex, so no worker/fork-thread order exists to invert
 * -- which is the whole reason a disk read on the worker cannot stall a HELLO.
 *
 * The queue lock is exec_kbackend.h §1b's exec_rxlock_t, the ONE receive-level
 * lock class (design §3.2.3 RULING, FC-P0.16; CONTRACT RULE 14.1). It is held
 * for a bounded handful of pointer moves plus one frame copy, is never held
 * across a protocol handler, a substrate call that may sleep, or a wake of
 * anything but the fork thread's own condition variable, and NOTHING is ever
 * acquired while it is held. The fork thread does not hold the fork mutex
 * while it waits on the queue (via exec_cv_wait_rx, thread context only), so
 * a snapshot reader in process context is never blocked behind an idle fork
 * thread.
 *
 * ------------------------------------------------------------------------
 * WHAT IS IN THIS FILE AND WHAT IS IN THE BINDING
 * ------------------------------------------------------------------------
 *
 *   vms_cluster_fork.c       PURE. Queues, dispatch, timer bookkeeping,
 *                            coalescing, stop/drain. Reaches the substrate ONLY
 *                            through `struct cf_ops`. Builds with a plain host
 *                            compiler (-DOVMX_CLUSTER_HOST) and is exercised by
 *                            tests/cluster/host/test_cluster_fork*.c -- rung R1.
 *   vms_cluster_fork_bind.c  GLUE. Binds cf_ops to exec_kbackend.h families §1b
 *                            (exec_rxlock, FC-P0.16), §2 (exec_cv), §4
 *                            (exec_zalloc), §7 (exec_mutex), §15
 *                            (exec_kthread) and §16 (exec_timer); owns every
 *                            substrate object by value.
 *
 * That split is design §3.9's own `_fsm.c` (pure) + `.c` (glue) pattern applied
 * to the fork module. It is deliberately NOT a `#ifdef OVMX_CLUSTER_HOST` inside
 * one file: conditional compilation would mean the host rung tests a DIFFERENT
 * translation unit from the one that ships, which is how "test-only real
 * behaviour" gets built. Design §3.2.2's leak table rows for §15/§16 name
 * "vms_cluster_fork.c only"; the fork MODULE is that TU plus its binding, and
 * the binding is the only half that names a seam type.
 *
 * INCLUDES: kernel-core headers only, and for the host rung the freestanding
 * ISO C type headers (tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_CLUSTER_FORK_H
#define OVMX_VMS_CLUSTER_FORK_H

/*
 * Fixed-width integer vocabulary. The same substrate-conditional TYPE-ONLY
 * block vms_cluster_codec.h carries, and for the same reason: this header and
 * vms_cluster_fork.c must compile with NO kernel headers for the R1 rung, while
 * still using the substrate's own <linux/types.h> / <sys/stdint.h> inside a
 * kmod. It selects type headers only -- never a facility header.
 */
#if defined(OVMX_KBACKEND_NETBSD) && defined(_KERNEL)
#  include <sys/types.h>
#  include <sys/stdint.h>
#elif defined(OVMX_KBACKEND_LINUX) || defined(__KERNEL__)
#  include <linux/types.h>
#else
#  include <stddef.h>
#  include <stdint.h>
#endif

struct vms_cluster;          /* vms_cluster.h -- the per-node context */
struct vms_cluster_fork;     /* opaque: defined in vms_cluster_fork.c */

/* ==========================================================================
 * 1. Status
 *
 * A module-local typed status, exactly like vms_cluster_codec.h's
 * vms_codec_status_t and for the same reason: this TU has no SS$_ vocabulary in
 * the host build.
 * ========================================================================== */
typedef enum cf_status {
	CF_OK = 0,
	CF_E_INVAL,      /* caller argument (null, owner out of range)          */
	CF_E_STOPPING,   /* the fork context is shutting down: no new work      */
	CF_E_NOBUF,      /* the pre-allocated pool is empty -- an HONEST drop   */
	CF_E_TOOBIG,     /* frame longer than the pool's buffer capacity        */
	CF_E_NOSLOT,     /* no free timer slot (never a silently unarmed timer) */
	CF_E_NOWORKER    /* the served-I/O worker is not running (FC-P6.6)      */
} cf_status_t;

const char *cf_status_name(cf_status_t st);

/* ==========================================================================
 * 1a. The core-owned receive buffer
 *
 * This is exec_kbackend.h §14's exec_lanbuf_t under the pure module's own name:
 * {data, cap, len}, allocated and freed by the CORE, never by the substrate --
 * the whole "Frame buffers" row of the design's leak table (the core never sees
 * an sk_buff or an mbuf because the buffer it receives into is its own). It is
 * spelled here rather than included from the seam because this header must
 * compile with NO kernel headers; vms_cluster_fork_bind.c carries the
 * _Static_asserts that hold the two spellings to ONE layout, so the compiler --
 * not a reader's memory -- keeps them from drifting.
 * ========================================================================== */
struct cf_lanbuf {
	uint8_t *data;
	uint32_t cap;
	uint32_t len;
};

/* ==========================================================================
 * 2. Who owns a piece of work
 *
 * The fork module knows nothing about protocol. It routes each work item to the
 * LAYER that posted it, through a handler that layer registered. `owner` is
 * that routing key -- one small table, not a switch ladder (design §3.9 rule 1).
 * ========================================================================== */
enum cf_owner {
	CF_OWNER_PE     = 0,   /* the LAN port driver / PEA0:  (vms_pe.c)      */
	CF_OWNER_SCS    = 1,   /* SCS: SBs, CDTs, credits      (vms_scs.c)     */
	CF_OWNER_CNXMAN = 2,   /* the connection manager       (vms_cnxman.c)  */
	CF_OWNER_DLM    = 3,   /* the lock manager's SCS arm   (vms_dlm_scs.c) */
	CF_OWNER_MSCP   = 4,   /* the MSCP disk SERVER         (vms_mscp_srv.c)*/
	/*
	 * The MSCP disk CLASS DRIVER (vms_mscp_cl.c, FC-P7.1). Its OWN owner
	 * and not a share of CF_OWNER_MSCP: a layer registers ONE work handler
	 * per owner (cf_set_work_handler), so a node that both serves and
	 * mounts -- the ordinary two-way cluster case -- would otherwise have
	 * the second starter silently replace the first's handler and stop its
	 * beat.
	 */
	CF_OWNER_MSCP_CL = 5,
	CF_OWNER__COUNT
};

/*
 * The reserved work KIND for a timer expiry. A layer's own work kinds are its
 * private vocabulary and start at 0; this one is the fork module's, so an
 * expiry arrives at the layer's handler exactly like any other work item and no
 * layer needs a second callback for timers.
 */
#define CF_WORK_TIMER 0xffffu

/*
 * The reserved work KIND for a SERVED-I/O WORKER COMPLETION (FC-P6.6). The
 * worker finished one cf_io request and the fork module posted the result back
 * to the layer that submitted it: arg0 == the layer's own request TAG (echoed
 * verbatim from struct cf_io), arg1 == the status the layer's own I/O callback
 * returned, ptr == NULL. Like CF_WORK_TIMER it arrives at the layer's ordinary
 * work handler, in the fork thread, under the fork mutex -- so a layer needs no
 * second callback and no completion of its own can run anywhere else.
 */
#define CF_WORK_IO_DONE 0xfffeu

/*
 * One unit of work: a continuation, the shape VMS queues to a CDT/PDT/BDT/RDT
 * when an operation cannot proceed (pp. 2-45/2-46). Fixed-width and small: the
 * pool is pre-allocated, because a timer callback MUST NOT allocate.
 *
 * For a timer expiry: kind == CF_WORK_TIMER, arg0 == the layer's timer id (e.g.
 * enum pe_timer), arg1 == the object key the layer armed it with, ptr == NULL.
 *
 * `ptr` is an OPTIONAL payload pointer whose lifetime belongs to the POSTING
 * layer -- the fork module copies the struct and never dereferences, frees or
 * interprets the pointer.
 */
struct cf_work {
	uint16_t owner;   /* enum cf_owner */
	uint16_t kind;    /* layer-private work code, or CF_WORK_TIMER */
	uint32_t arg0;
	uint32_t arg1;
	void    *ptr;
};

/* ==========================================================================
 * 3. The injected ops -- the ONLY way the pure module reaches the world
 *
 * Production binds these to exec_kbackend.h (vms_cluster_fork_bind.c); a host
 * test binds them to fakes; the rung-2 simulator binds them to its own
 * scheduler. There is no `now`/`log` op here on purpose: this module makes no
 * time-dependent decision and must not print from receive or timer context --
 * what it has to say, it says in counters (cf_stats), which is INV-6-honest and
 * cannot flood a console at rebuild message rates.
 * ========================================================================== */
struct cf_ops {
	/* The QUEUE LOCK -- exec_kbackend.h §1b's exec_rxlock_t (CONTRACT RULE
	 * 14.1). Acquired in receive context, timer context, process context
	 * and the fork thread; held only for the bounded critical sections in
	 * vms_cluster_fork.c. */
	void (*lock)(void *ctx);
	void (*unlock)(void *ctx);

	/* Wait / wake, paired with THAT lock (exec_kbackend.h §1b's
	 * exec_cv_wait_rx / §2's exec_cv_signal-broadcast: waiter and waker
	 * must share the lock or wakeups are lost). `wait` is called with the
	 * lock held (thread context only), atomically drops it, sleeps, and
	 * returns with it held again; nonzero means "interrupted", and the
	 * caller re-tests its predicate first. `wake` is called with the lock
	 * held, and is legal from receive level. */
	int  (*wait)(void *ctx);
	void (*wake)(void *ctx);

	/* The FORK MUTEX -- VMS's fork-IPL serialisation. Sleepable (§7
	 * exec_mutex_t), held for the whole of each dispatched event and by a
	 * snapshot reader. Never held while waiting on the queue. */
	void (*fork_lock)(void *ctx);
	void (*fork_unlock)(void *ctx);

	/* One substrate timer per SLOT INDEX. `arm` is called with the queue
	 * lock HELD (it must not sleep: mod_timer / callout_schedule), and
	 * re-arming an armed timer MOVES it -- it never stacks. `cancel` is
	 * called with NO lock held because it MAY SLEEP: it must wait out a
	 * callback already running (del_timer_sync / callout_halt), so that a
	 * cancelled timer can never touch state the caller is about to free. */
	void (*timer_arm)(void *ctx, uint32_t slot, uint32_t ms);
	void (*timer_cancel)(void *ctx, uint32_t slot);

	/* Nonzero iff the substrate has asked the fork thread to stop
	 * (exec_kthread_should_stop). Tested once per wait iteration, in
	 * addition to this module's own stop flag -- the module's flag is what
	 * a cf_request_stop() sets, and it is what makes the cv wait terminate,
	 * because neither substrate's kthread-stop wakes a cv sleeper by
	 * itself. */
	int  (*should_stop)(void *ctx);

	/*
	 * The SERVED-I/O WORKER's own wait / wake / stop test (FC-P6.6). These
	 * three are OPTIONAL AS A GROUP: a binding that does not provide them
	 * simply has no worker, and cf_io_post() then answers CF_E_NOWORKER --
	 * an honest refusal the submitting layer turns into a real error
	 * status, never a synchronous disk call smuggled onto the fork thread.
	 *
	 * `io_wait` is a SECOND exec_cv_t paired with THE SAME queue lock, for
	 * the same lost-wakeup-free reason `wait`/`wake` are paired: waiter and
	 * waker must share the interlock. A second cv rather than a share of
	 * the first is deliberate -- every received frame wakes the first one,
	 * and waking an idle worker thousands of times a second during a
	 * rebuild is precisely the jitter this worker exists to remove.
	 */
	int  (*io_wait)(void *ctx);
	void (*io_wake)(void *ctx);
	int  (*io_should_stop)(void *ctx);

	/* Pool allocation, at create/destroy time only, from process context.
	 * Production: exec_zalloc / exec_free (§4). NEVER called from receive
	 * or timer context -- that is the whole reason the pools exist. */
	void *(*alloc)(void *ctx, uint32_t n);
	void  (*free)(void *ctx, void *p);

	void *ctx;
};

/* ==========================================================================
 * 4. Sizing
 *
 * Every pool is fixed and pre-allocated at CLUSTER_START. A zero field takes
 * the default. Exhaustion is an HONEST, COUNTED drop -- never a blocked
 * softirq, never an allocation in the wrong context: "a dropped frame is a
 * retransmit, a blocked softirq is a dead node" (exec_kbackend.h RULE 1).
 * ========================================================================== */
#define CF_RX_BUFS_DEFAULT      64u   /* receive buffers in the pool           */
#define CF_RX_CAP_DEFAULT     1518u   /* bytes per buffer (a full Ethernet     */
                                      /* frame; FC-P0.9 passes NISCS_MAX_PKTSZ */
                                      /* clamped to the interface MTU)         */
#define CF_WORK_ITEMS_DEFAULT  128u   /* work continuations in the pool        */
#define CF_TIMER_SLOTS_DEFAULT  64u   /* distinct (owner,which,key) timers     */
/*
 * Served-I/O requests that can be QUEUED to the worker at once (FC-P6.6). Small
 * on purpose: the only submitter today is the MSCP server, which has
 * MSCP_SRV_MAX_REQS (4) outstanding remote requests by construction, so 16 is
 * four times the whole ceiling plus room for a second submitter. Exhaustion is
 * an honest CF_E_NOBUF the submitter answers with a real error -- never a
 * synchronous fallback.
 */
#define CF_IO_ITEMS_DEFAULT     16u

struct cf_config {
	uint32_t rx_bufs;
	uint32_t rx_cap;
	uint32_t work_items;
	uint32_t timer_slots;
	uint32_t io_items;
};

/* Fill `out` with `in`, substituting the default for each zero field. The glue
 * needs the SAME effective numbers the pure module will use -- it allocates one
 * substrate timer per slot -- so the defaulting rule lives in one function
 * rather than in two places that agree until someone edits one. */
void cf_config_normalize(struct cf_config *out, const struct cf_config *in);

/* ==========================================================================
 * 5. Counters -- the module's honest diagnostic surface
 *
 * Fixed-width, monotonic, and every one of them counts a REAL event this module
 * observed. They are what CLUSTER_DIAG reports and what the host tests assert
 * on; there is no field here that is not incremented by a line of this module.
 * ========================================================================== */
struct cf_stats {
	uint64_t rx_enqueued;        /* frames copied into the pool and queued  */
	uint64_t rx_dropped_nobuf;   /* pool empty in receive context           */
	uint64_t rx_dropped_toobig;  /* frame longer than a pool buffer         */
	uint64_t rx_refused_stopping;/* frame arrived after stop was requested  */
	uint64_t rx_dispatched;      /* frames handed to the port handler       */
	uint64_t rx_undeliverable;   /* dequeued with no rx handler registered  */

	uint64_t work_posted;        /* accepted cf_post + timer expiries       */
	uint64_t work_refused_stopping;
	uint64_t work_dropped_nobuf; /* work-item pool empty                    */
	uint64_t work_dispatched;
	uint64_t work_undeliverable; /* no handler registered for that owner    */

	/*
	 * Every cf_timer_expired() call lands in EXACTLY ONE of the four
	 * counters below, so a caller can prove no substrate callback was
	 * silently lost:
	 *   fires == timer_expiries + timer_coalesced + timer_stale
	 *            + timer_dropped_nobuf
	 */
	uint64_t timer_arms;
	uint64_t timer_cancels;
	uint64_t timer_expiries;      /* callbacks that posted a work item      */
	uint64_t timer_coalesced;     /* expiry while one was already queued    */
	uint64_t timer_stale;         /* callback for a disarmed/released slot  */
	uint64_t timer_dropped_nobuf; /* work pool empty (or a stop in progress)*/
	uint64_t timer_dequeued;      /* queued expiries removed by a cancel    */

	uint64_t waits;              /* times the fork thread slept             */
	uint64_t wait_interrupts;    /* wait returned "interrupted"             */

	/*
	 * The SERVED-I/O WORKER (FC-P6.6). Every cf_io_post lands in exactly one
	 * of the first four, so a caller can prove no submission was lost:
	 *   posts == io_posted + io_refused_stopping + io_refused_noworker
	 *            + io_dropped_nobuf
	 * and every ACCEPTED request lands in exactly one of the last three:
	 *   io_posted == io_completed + io_abandoned + io_completion_dropped
	 *                (+ at most one still in flight)
	 */
	uint64_t io_posted;              /* accepted cf_io_post                 */
	uint64_t io_refused_stopping;    /* posted after a stop was requested   */
	uint64_t io_refused_noworker;    /* no worker ops / no handler bound    */
	uint64_t io_dropped_nobuf;       /* the io-item pool OR the work pool
					  * (the reserved completion) was empty */
	uint64_t io_started;             /* requests handed to a layer callback */
	uint64_t io_completed;           /* completions posted to the fork queue*/
	uint64_t io_abandoned;           /* queued requests dropped by a stop   */
	uint64_t io_completion_dropped;  /* the whole context stopped under an
					  * in-flight request: the ONLY way an
					  * accepted request goes unanswered   */
	uint64_t io_waits;               /* times the worker slept              */

	uint32_t rx_free;            /* buffers currently free (INSTANTANEOUS)  */
	uint32_t work_free;          /* work items currently free               */
	uint32_t io_free;            /* io items currently free                 */
	uint32_t timer_slots_used;   /* slots currently allocated               */
	uint32_t stopping;           /* nonzero once a stop was requested       */
	uint32_t io_stopping;        /* nonzero once the worker was asked to go */
};

/* ==========================================================================
 * 6. Handlers -- how a layer receives its events
 *
 * Both run IN THE FORK THREAD, UNDER THE FORK MUTEX. A handler may call
 * cf_post/cf_timer_arm/cf_timer_cancel (they are re-entrant with respect to the
 * fork mutex, which they never take) and may block on the lock manager's own
 * locks in the documented order. It must never call cf_run, cf_destroy, or
 * vms_cluster_fork_enter.
 * ========================================================================== */

/* A received frame: the COMPLETE Ethernet frame (byte 0 = destination MAC),
 * valid only for the duration of the call -- the buffer returns to the pool as
 * soon as the handler returns, so a layer that must keep bytes copies them. */
typedef void (*cf_rx_handler_t)(void *ctx, const uint8_t *frame, uint32_t len);

/* One work continuation. `w` is valid only for the duration of the call. */
typedef void (*cf_work_handler_t)(void *ctx, const struct cf_work *w);

/* ==========================================================================
 * 7. The pure module's API (vms_cluster_fork.c)
 * ========================================================================== */

/* Create the fork context. Allocates every pool through ops->alloc, from
 * process context. Returns NULL if `ops` is incomplete or an allocation fails
 * -- there is no half-built fork context. */
struct vms_cluster_fork *cf_create(const struct cf_ops *ops,
				   const struct cf_config *cfg);

/* Free everything cf_create allocated. The caller MUST already have stopped and
 * JOINED the fork thread and cancelled every timer (vms_cluster_fork_stop does
 * exactly that): this function does not, and cannot, wait for another context. */
void cf_destroy(struct vms_cluster_fork *f);

/* Register the port's receive handler / one layer's work handler. Called from
 * process context at CLUSTER_START, before the thread runs. A second call
 * replaces the first; a NULL handler unregisters (its events are then counted
 * as undeliverable rather than dropped silently). */
void cf_set_rx_handler(struct vms_cluster_fork *f, cf_rx_handler_t cb, void *ctx);
cf_status_t cf_set_work_handler(struct vms_cluster_fork *f, enum cf_owner owner,
				cf_work_handler_t cb, void *ctx);

/* ---- (1) RECEIVE context. CONTRACT RULE 1: copy, enqueue, wake. --------- */
cf_status_t cf_rx_deliver(struct vms_cluster_fork *f,
			  const uint8_t *frame, uint32_t len);

/* ---- (2) TIMER context. CONTRACT RULE 2: post and wake, nothing else. ---
 * `slot` is the index the binding was given by ops->timer_arm. */
void cf_timer_expired(struct vms_cluster_fork *f, uint32_t slot);

/* ---- (3) PROCESS or FORK context. -------------------------------------- */

/* Post one work continuation. Copies `*w`. */
cf_status_t cf_post(struct vms_cluster_fork *f, const struct cf_work *w);

/* Arm the timer identified by (owner, which, key) to fire once in `ms`
 * milliseconds; re-arming an armed identity MOVES it, never stacks. Returns
 * CF_E_NOSLOT when the slot table is full -- an honest failure the caller must
 * handle, never a silently unarmed timer.
 *
 * PRECONDITION: cf_timer_arm and cf_timer_cancel are called from the FORK
 * THREAD (a handler), or from process context before the thread starts / after
 * it has joined. They are NOT mutually safe to call concurrently for the same
 * identity, and they do not need to be: all protocol runs in the one context,
 * which is the entire point of this module. */
cf_status_t cf_timer_arm(struct vms_cluster_fork *f, enum cf_owner owner,
			 uint32_t which, uint32_t key, uint32_t ms);

/* Disarm (owner, which, key), wait out a callback already running, DISCARD any
 * expiry it already queued, and release the slot. After it returns, no expiry
 * for that identity can be dispatched -- so a layer may free the object the
 * timer named. MAY SLEEP (ops->timer_cancel does). */
void cf_timer_cancel(struct vms_cluster_fork *f, enum cf_owner owner,
		     uint32_t which, uint32_t key);

/* ---- The fork thread body ---------------------------------------------- */

/* Dispatch AT MOST ONE queued event under the fork mutex. Returns 1 if one was
 * dispatched, 0 if both queues were empty. Never blocks on the queue. This is
 * the whole dispatch step, exposed so a host test (and the rung-2 simulator)
 * can drive the module deterministically with no thread at all. */
int cf_dispatch_one(struct vms_cluster_fork *f);

/* The fork thread's body: wait for an event, dispatch it, repeat. Returns only
 * after a stop has been requested AND both queues have been drained -- work
 * already queued when the stop arrived is RUN, not thrown away, so a last-gasp
 * or teardown item posted before the stop still reaches its layer. */
void cf_run(struct vms_cluster_fork *f);

/* Ask cf_run to finish: refuse new work, wake the thread, let it drain. Safe
 * from any context. Idempotent. */
void cf_request_stop(struct vms_cluster_fork *f);

/* Copy the counters out. Safe from any context (taken under the queue lock). */
void cf_stats_get(struct vms_cluster_fork *f, struct cf_stats *out);

/* ==========================================================================
 * 7a. THE SERVED-I/O WORKER (design §3.2.6 E42 corollary, FC-P6.6)
 *
 * WHY IT EXISTS. exec_kbackend.h §8's block ops are contractually synchronous
 * and "MAY SLEEP; call only from process context with no exec_lock held". The
 * MSCP disk SERVER has to make them: a served READ is a real read of a real
 * volume. Making them on the FORK THREAD stalls the one context that also owns
 * the HELLO cadence, the VC retransmit ladder and every barrier step -- a 20 ms
 * disk read is a 20 ms cluster stall on this node and, through the barrier, on
 * every other member. VMS does not do that: its MSCP server issues local I/O
 * asynchronously (an IRP to the local driver) and completes the MSCP command on
 * that I/O's completion; the port is never stalled.
 *
 * THE SHAPE. A layer registers an I/O callback (cf_set_io_handler) and submits
 * requests (cf_io_post). The WORKER thread pops one at a time, runs the
 * callback with NO lock held -- that is the one place in the cluster stack a
 * blocking substrate call is legal -- and the fork module posts the callback's
 * status back to the SAME layer as an ordinary CF_WORK_IO_DONE work item, which
 * the layer's work handler receives in the fork thread under the fork mutex.
 * So the protocol (the MSCP end message, the SEND DATA) is still built in the
 * one serialised context, and only the disk wait moved.
 *
 * WHAT THE WORKER MAY NOT DO. It runs no protocol, touches no PE/SCS/CNXMAN/
 * DLM/MSCP state, and never takes the fork mutex. Everything it is given
 * arrives in the copied struct cf_io, and everything it has to say is the
 * uint32_t it returns.
 * ========================================================================== */

/*
 * One submitted I/O. The fork module COPIES it and interprets exactly two
 * fields: `owner` (which callback runs it, and which layer gets the completion)
 * and nothing else -- `op`/`tag`/`arg0..3`/`ptr` are the layer's private
 * vocabulary, carried verbatim. `ptr`'s lifetime belongs to the SUBMITTING
 * layer, which must keep it alive until the completion arrives (the MSCP server
 * does exactly that: an HRB owns its staging slot for its whole life).
 */
struct cf_io {
	uint16_t owner;   /* enum cf_owner */
	uint16_t op;      /* layer-private operation code */
	uint32_t tag;     /* layer-private request tag, echoed in the completion */
	uint32_t arg0;
	uint32_t arg1;
	uint32_t arg2;
	uint32_t arg3;
	void    *ptr;
};

/*
 * The worker-context callback. Runs on the SERVED-I/O WORKER THREAD, in process
 * context, with NO queue lock and NO fork mutex held, and it MAY SLEEP. Its
 * return value is the layer's own status word and is delivered VERBATIM as the
 * completion's arg1 -- the fork module never invents, rewrites or defaults it,
 * so a served READ's success or failure on the wire is the block layer's real
 * answer (INV-6).
 */
typedef uint32_t (*cf_io_handler_t)(void *ctx, const struct cf_io *io);

/* Register / unregister one owner's I/O callback. Process context, at layer
 * start-up. Registering a non-NULL callback also CLEARS any previous stop
 * request for the worker, so a layer that stopped and restarted is served
 * again. A NULL callback makes that owner's submissions CF_E_NOWORKER. */
cf_status_t cf_set_io_handler(struct vms_cluster_fork *f, enum cf_owner owner,
			      cf_io_handler_t cb, void *ctx);

/*
 * Submit one I/O to the worker. Fork thread or process context; never receive
 * or timer context (it may find a pool empty, which is a counter, not a sleep).
 *
 * Returns CF_OK when the worker ACCEPTED it -- and then exactly one
 * CF_WORK_IO_DONE WILL be delivered for it unless the whole fork context is
 * stopped first, because the completion's work item is RESERVED here, with the
 * request. That is deliberate: a layer that hands over a transfer parks a
 * request until the answer comes back, so a completion that could be refused
 * for want of a work item would park it forever. CF_E_NOBUF means either pool
 * was empty -- an honest refusal the caller answers now, instead of a stuck
 * request it can never answer.
 */
cf_status_t cf_io_post(struct vms_cluster_fork *f, const struct cf_io *io);

/* Clear the worker's stop flag. Called by the binding before it starts the
 * worker thread, so a stop/start cycle works. */
void cf_io_start(struct vms_cluster_fork *f);

/* Ask the worker to finish: refuse new submissions, wake it, let it exit.
 * QUEUED requests are ABANDONED (counted in io_abandoned) rather than run --
 * the layer that submitted them is being torn down and there is nobody left to
 * answer. A request already inside a callback runs to completion first, which
 * is what makes it safe for the layer to free the buffer it lent AFTER the
 * worker thread has been joined. Safe from any context; idempotent. */
void cf_io_request_stop(struct vms_cluster_fork *f);

/* The worker thread's body: wait for a request, run it, post the completion,
 * repeat. Returns once a worker stop has been requested and the queue is
 * empty. Exposed like cf_run so a host test can drive it with no thread. */
void cf_io_run(struct vms_cluster_fork *f);

/* Run AT MOST ONE queued I/O request (pop, callback, post completion). Returns
 * 1 if one ran, 0 if the queue was empty. The deterministic half of cf_io_run,
 * for the R1 rung and the rung-2 simulator. */
int cf_io_run_one(struct vms_cluster_fork *f);

/* The `ctx` the creator injected in its ops. This exists so the GLUE can find
 * its own binding from the `struct vms_cluster_fork *` the per-node context
 * publishes, instead of adding a second pointer to the frozen FC-P0.1
 * `struct vms_cluster`. Nobody else has any business calling it: the pure
 * module never dereferences the value. */
void *cf_ops_ctx(struct vms_cluster_fork *f);

/* ==========================================================================
 * 8. The executive glue's API (vms_cluster_fork_bind.c)
 *
 * These return an SS$_ status as an int, the same convention vms_pe.h uses.
 * ========================================================================== */

/* Allocate the fork context and its substrate objects, then start the ONE
 * "CNXMAN fork" kernel thread for this node and publish it as cl->fork. On any
 * failure nothing is left behind and cl->fork stays NULL; SS$_NOSUCHDEV when
 * the substrate has no kthread binding yet, which is the honest end of the road
 * (Rule 9). Process context; MAY SLEEP. */
int vms_cluster_fork_start(struct vms_cluster *cl, const struct cf_config *cfg);

/* Request the stop, JOIN the thread, cancel and destroy every timer, free
 * everything, clear cl->fork. Idempotent. Process context; MAY SLEEP; never
 * called from the fork thread itself. */
void vms_cluster_fork_stop(struct vms_cluster *cl);

/*
 * Start / stop the SERVED-I/O WORKER thread for this node (FC-P6.6). Separate
 * from vms_cluster_fork_start because serving disks is a ROLE: a node with
 * MSCP_LOAD/MSCP_SERVE_ALL off never needs the thread, and VMS likewise loads
 * its MSCP server conditionally. The MSCP server's own start/stop call these.
 *
 * _stop JOINS the thread, so after it returns no callback is running and the
 * caller may free every buffer it lent the worker. Both are idempotent, both
 * are process context, both MAY SLEEP, and neither may be called from the fork
 * thread or from the worker itself. SS$_NOSUCHDEV from _start is the honest end
 * of the road on a substrate with no §15 binding (Rule 9).
 */
int  vms_cluster_fork_worker_start(struct vms_cluster *cl);
void vms_cluster_fork_worker_stop(struct vms_cluster *cl);

/* Take / drop the fork mutex around a snapshot read from process context, so
 * $GETSYI and the CLUSTER_DIAG ioctls see the databases between two dispatched
 * events rather than in the middle of one. MAY SLEEP. Never called from a
 * handler (the fork thread already holds the mutex there). */
void vms_cluster_fork_enter(struct vms_cluster *cl);
void vms_cluster_fork_leave(struct vms_cluster *cl);

#endif /* OVMX_VMS_CLUSTER_FORK_H */
