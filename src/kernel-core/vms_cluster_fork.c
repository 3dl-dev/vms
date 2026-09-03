// SPDX-License-Identifier: GPL-2.0
/*
 * vms_cluster_fork.c - the PURE half of the cluster fork context (FC-P0.5):
 * the two queues, the one-at-a-time dispatch, the timer bookkeeping, and the
 * stop/drain rule. Read vms_cluster_fork.h first -- it carries the contract,
 * the three calling contexts, the lock order and the public-document grounding.
 *
 * PURE means: no substrate include, no libc call, no global, no allocation
 * except through ops->alloc, and no decision that depends on which kernel this
 * is. Everything this file needs from the world arrives through `struct cf_ops`
 * (design §3.9 rules 3 and 6), which is what lets tests/cluster/host build and
 * run it with a plain host compiler -- rung R1 -- and what will let the rung-2
 * N-node simulator instantiate it once per simulated node.
 *
 * The whole file is a queue discipline. It contains no protocol: it does not
 * parse a byte, does not know an ethertype, and never inspects a work item's
 * `kind` except to recognise the one kind it owns, CF_WORK_TIMER.
 */

#include "vms_cluster_fork.h"

/*
 * A null pointer constant, spelled locally. The NetBSD kernel branch of this
 * header's type block (<sys/types.h> + <sys/stdint.h>) does NOT define NULL,
 * and a freestanding cluster TU does not get to reach for <stddef.h> to fix
 * that -- the elf32-vax cross build caught exactly this. So the module names
 * the constant itself and depends on no header for it.
 */
#define CF_NULL ((void *)0)

/* ------------------------------------------------------------------ *
 * Local byte moves (no libc, no kernel string.h) -- the same posture
 * vms_cluster_codec.c takes, for the same portability reason.
 * ------------------------------------------------------------------ */

static void cf_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		dst[i] = src[i];
}

static void cf_zero(void *dst, uint32_t n)
{
	uint8_t *p = (uint8_t *)dst;
	uint32_t i;

	for (i = 0; i < n; i++)
		p[i] = 0;
}

const char *cf_status_name(cf_status_t st)
{
	switch (st) {
	case CF_OK:          return "CF_OK";
	case CF_E_INVAL:     return "CF_E_INVAL";
	case CF_E_STOPPING:  return "CF_E_STOPPING";
	case CF_E_NOBUF:     return "CF_E_NOBUF";
	case CF_E_TOOBIG:    return "CF_E_TOOBIG";
	case CF_E_NOSLOT:    return "CF_E_NOSLOT";
	case CF_E_NOWORKER:  return "CF_E_NOWORKER";
	}
	return "CF_E_?";
}

/* ==================================================================== *
 * 1. The objects
 * ==================================================================== */

/*
 * A queued receive buffer: the core-owned struct cf_lanbuf (== the seam's
 * exec_lanbuf_t, asserted in vms_cluster_fork_bind.c) with a queue link
 * appended. The buffer is EMBEDDED, not pointed at, so a pool entry and its
 * queue link are one allocation.
 */
struct cf_rxbuf {
	struct cf_lanbuf b;
	struct cf_rxbuf *next;
};

/* A queued work continuation. `timer_slot` is slot+1 for an expiry posted by
 * cf_timer_expired and 0 for everything else -- it is what lets a cancel pull
 * an already-queued expiry back off the queue. */
struct cf_witem {
	struct cf_work   w;
	uint32_t         timer_slot;
	struct cf_witem *next;
};

/* One timer identity. `armed` and `pending` are separate facts: a one-shot
 * timer that has fired is no longer armed but its expiry may still be queued,
 * and a cancel must deal with both. */
struct cf_tslot {
	uint32_t which;
	uint32_t key;
	uint16_t owner;
	uint8_t  in_use;
	uint8_t  armed;
	uint8_t  pending;
	uint8_t  pad[3];
};

/*
 * A queued served-I/O request (FC-P6.6): the layer's copied struct cf_io with a
 * queue link appended, exactly like a work continuation -- plus the work item
 * its COMPLETION will be posted in.
 *
 * WHY THE COMPLETION IS RESERVED AT SUBMIT TIME. A layer that hands over a
 * transfer parks a request until the answer comes back (the MSCP server's HRB
 * holds its staging slot for exactly that long). If the completion could be
 * refused for want of a work item, an ACCEPTED request would never be answered
 * and that park would be permanent -- the one failure mode this queue must not
 * have. So the item is taken WITH the request or the request is not accepted at
 * all: a refusal the submitter can answer, instead of a stuck request it
 * cannot. (The same reasoning as the HRB owning its staging slot for its whole
 * life, one layer down.)
 */
struct cf_ioitem {
	struct cf_io      io;
	struct cf_witem  *done;
	struct cf_ioitem *next;
};

struct cf_rxq { struct cf_rxbuf *head, *tail; };
struct cf_wq  { struct cf_witem *head, *tail; };
struct cf_ioq { struct cf_ioitem *head, *tail; };

struct vms_cluster_fork {
	struct cf_ops    ops;
	struct cf_config cfg;

	/* the pools, one allocation each, never grown after cf_create */
	struct cf_rxbuf *rxbufs;
	uint8_t         *rxdata;
	struct cf_witem *witems;
	struct cf_tslot *tslots;
	struct cf_ioitem *ioitems;

	struct cf_rxq rx_free;
	struct cf_rxq rx_ready;
	struct cf_wq  w_free;
	struct cf_wq  w_ready;
	struct cf_ioq io_free;
	struct cf_ioq io_ready;

	cf_rx_handler_t   rx_cb;
	void             *rx_ctx;
	cf_work_handler_t work_cb[CF_OWNER__COUNT];
	void             *work_ctx[CF_OWNER__COUNT];
	cf_io_handler_t   io_cb[CF_OWNER__COUNT];
	void             *io_ctx[CF_OWNER__COUNT];

	uint32_t n_rx_free;
	uint32_t n_w_free;
	uint32_t n_io_free;
	uint32_t n_slots;

	uint8_t serve_rx_next;   /* round-robin cursor, see cf_take_event */
	uint8_t stopping;
	uint8_t io_stopping;     /* the WORKER's own stop, independent of the
				  * context's: an MSCP server can stop serving
				  * without the node leaving the cluster    */

	struct cf_stats st;
};

/* ==================================================================== *
 * 2. Queue primitives (all callers hold the queue lock)
 * ==================================================================== */

static void rxq_push(struct cf_rxq *q, struct cf_rxbuf *b)
{
	b->next = CF_NULL;
	if (q->tail)
		q->tail->next = b;
	else
		q->head = b;
	q->tail = b;
}

static struct cf_rxbuf *rxq_pop(struct cf_rxq *q)
{
	struct cf_rxbuf *b = q->head;

	if (!b)
		return CF_NULL;
	q->head = b->next;
	if (!q->head)
		q->tail = CF_NULL;
	b->next = CF_NULL;
	return b;
}

static void wq_push(struct cf_wq *q, struct cf_witem *it)
{
	it->next = CF_NULL;
	if (q->tail)
		q->tail->next = it;
	else
		q->head = it;
	q->tail = it;
}

static struct cf_witem *wq_pop(struct cf_wq *q)
{
	struct cf_witem *it = q->head;

	if (!it)
		return CF_NULL;
	q->head = it->next;
	if (!q->head)
		q->tail = CF_NULL;
	it->next = CF_NULL;
	return it;
}

static void ioq_push(struct cf_ioq *q, struct cf_ioitem *it)
{
	it->next = CF_NULL;
	if (q->tail)
		q->tail->next = it;
	else
		q->head = it;
	q->tail = it;
}

static struct cf_ioitem *ioq_pop(struct cf_ioq *q)
{
	struct cf_ioitem *it = q->head;

	if (!it)
		return CF_NULL;
	q->head = it->next;
	if (!q->head)
		q->tail = CF_NULL;
	it->next = CF_NULL;
	return it;
}

/* ==================================================================== *
 * 3. Construction
 * ==================================================================== */

static int cf_ops_complete(const struct cf_ops *o)
{
	return o && o->lock && o->unlock && o->wait && o->wake &&
	       o->fork_lock && o->fork_unlock &&
	       o->timer_arm && o->timer_cancel && o->should_stop &&
	       o->alloc && o->free;
}

void cf_config_normalize(struct cf_config *c, const struct cf_config *in)
{
	if (!c)
		return;
	c->rx_bufs     = (in && in->rx_bufs)     ? in->rx_bufs     : CF_RX_BUFS_DEFAULT;
	c->rx_cap      = (in && in->rx_cap)      ? in->rx_cap      : CF_RX_CAP_DEFAULT;
	c->work_items  = (in && in->work_items)  ? in->work_items  : CF_WORK_ITEMS_DEFAULT;
	c->timer_slots = (in && in->timer_slots) ? in->timer_slots : CF_TIMER_SLOTS_DEFAULT;
	c->io_items    = (in && in->io_items)    ? in->io_items    : CF_IO_ITEMS_DEFAULT;
}

static int cf_build_rx_pool(struct vms_cluster_fork *f)
{
	uint32_t i;

	f->rxbufs = (struct cf_rxbuf *)f->ops.alloc(f->ops.ctx,
			f->cfg.rx_bufs * (uint32_t)sizeof(*f->rxbufs));
	f->rxdata = (uint8_t *)f->ops.alloc(f->ops.ctx,
			f->cfg.rx_bufs * f->cfg.rx_cap);
	if (!f->rxbufs || !f->rxdata)
		return 0;

	for (i = 0; i < f->cfg.rx_bufs; i++) {
		f->rxbufs[i].b.data = f->rxdata + (size_t)i * f->cfg.rx_cap;
		f->rxbufs[i].b.cap  = f->cfg.rx_cap;
		f->rxbufs[i].b.len  = 0;
		rxq_push(&f->rx_free, &f->rxbufs[i]);
	}
	f->n_rx_free = f->cfg.rx_bufs;
	return 1;
}

static int cf_build_work_pool(struct vms_cluster_fork *f)
{
	uint32_t i;

	f->witems = (struct cf_witem *)f->ops.alloc(f->ops.ctx,
			f->cfg.work_items * (uint32_t)sizeof(*f->witems));
	if (!f->witems)
		return 0;

	for (i = 0; i < f->cfg.work_items; i++) {
		cf_zero(&f->witems[i], (uint32_t)sizeof(f->witems[i]));
		wq_push(&f->w_free, &f->witems[i]);
	}
	f->n_w_free = f->cfg.work_items;
	return 1;
}

static int cf_build_io_pool(struct vms_cluster_fork *f)
{
	uint32_t i;

	f->ioitems = (struct cf_ioitem *)f->ops.alloc(f->ops.ctx,
			f->cfg.io_items * (uint32_t)sizeof(*f->ioitems));
	if (!f->ioitems)
		return 0;

	for (i = 0; i < f->cfg.io_items; i++) {
		cf_zero(&f->ioitems[i], (uint32_t)sizeof(f->ioitems[i]));
		ioq_push(&f->io_free, &f->ioitems[i]);
	}
	f->n_io_free = f->cfg.io_items;
	return 1;
}

static int cf_build_timer_slots(struct vms_cluster_fork *f)
{
	f->tslots = (struct cf_tslot *)f->ops.alloc(f->ops.ctx,
			f->cfg.timer_slots * (uint32_t)sizeof(*f->tslots));
	if (!f->tslots)
		return 0;
	cf_zero(f->tslots, f->cfg.timer_slots * (uint32_t)sizeof(*f->tslots));
	return 1;
}

struct vms_cluster_fork *cf_create(const struct cf_ops *ops,
				   const struct cf_config *cfg)
{
	struct vms_cluster_fork *f;

	if (!cf_ops_complete(ops))
		return CF_NULL;

	f = (struct vms_cluster_fork *)ops->alloc(ops->ctx,
			(uint32_t)sizeof(*f));
	if (!f)
		return CF_NULL;
	cf_zero(f, (uint32_t)sizeof(*f));
	f->ops = *ops;
	cf_config_normalize(&f->cfg, cfg);
	f->serve_rx_next = 1;   /* frames first on the very first dispatch */

	if (!cf_build_rx_pool(f) || !cf_build_work_pool(f) ||
	    !cf_build_io_pool(f) || !cf_build_timer_slots(f)) {
		cf_destroy(f);
		return CF_NULL;
	}
	return f;
}

void cf_destroy(struct vms_cluster_fork *f)
{
	struct cf_ops ops;

	if (!f)
		return;
	ops = f->ops;
	if (f->rxdata)
		ops.free(ops.ctx, f->rxdata);
	if (f->rxbufs)
		ops.free(ops.ctx, f->rxbufs);
	if (f->witems)
		ops.free(ops.ctx, f->witems);
	if (f->ioitems)
		ops.free(ops.ctx, f->ioitems);
	if (f->tslots)
		ops.free(ops.ctx, f->tslots);
	ops.free(ops.ctx, f);
}

void cf_set_rx_handler(struct vms_cluster_fork *f, cf_rx_handler_t cb, void *ctx)
{
	if (!f)
		return;
	f->ops.lock(f->ops.ctx);
	f->rx_cb  = cb;
	f->rx_ctx = ctx;
	f->ops.unlock(f->ops.ctx);
}

cf_status_t cf_set_work_handler(struct vms_cluster_fork *f, enum cf_owner owner,
				cf_work_handler_t cb, void *ctx)
{
	if (!f || (unsigned)owner >= (unsigned)CF_OWNER__COUNT)
		return CF_E_INVAL;
	f->ops.lock(f->ops.ctx);
	f->work_cb[owner]  = cb;
	f->work_ctx[owner] = ctx;
	f->ops.unlock(f->ops.ctx);
	return CF_OK;
}

/* ==================================================================== *
 * 4. Receive context -- CONTRACT RULE 1
 * ==================================================================== */

/*
 * Copy the frame into a pool buffer, queue it, wake the fork thread. That is
 * the entire permitted repertoire of a receive callback: no allocation, no
 * sleep, no fork mutex, no protocol. When the pool is empty the frame is
 * DROPPED AND COUNTED, because a dropped frame is a retransmit and a blocked
 * softirq is a dead node (exec_kbackend.h RULE 1).
 */
cf_status_t cf_rx_deliver(struct vms_cluster_fork *f,
			  const uint8_t *frame, uint32_t len)
{
	struct cf_rxbuf *b;

	if (!f || !frame || len == 0)
		return CF_E_INVAL;

	f->ops.lock(f->ops.ctx);
	if (f->stopping) {
		f->st.rx_refused_stopping++;
		f->ops.unlock(f->ops.ctx);
		return CF_E_STOPPING;
	}
	if (len > f->cfg.rx_cap) {   /* NISCS_MAX_PKTSZ clamp: honest, counted */
		f->st.rx_dropped_toobig++;
		f->ops.unlock(f->ops.ctx);
		return CF_E_TOOBIG;
	}
	b = rxq_pop(&f->rx_free);
	if (!b) {
		f->st.rx_dropped_nobuf++;
		f->ops.unlock(f->ops.ctx);
		return CF_E_NOBUF;
	}
	f->n_rx_free--;

	cf_copy(b->b.data, frame, len);
	b->b.len = len;
	rxq_push(&f->rx_ready, b);
	f->st.rx_enqueued++;
	f->ops.wake(f->ops.ctx);      /* under the lock: the cv contract */
	f->ops.unlock(f->ops.ctx);
	return CF_OK;
}

/* ==================================================================== *
 * 5. Posting work
 * ==================================================================== */

/* Queue one continuation. Caller holds the queue lock. `timer_slot` is slot+1
 * for an expiry, 0 otherwise. */
static cf_status_t cf_enqueue_locked(struct vms_cluster_fork *f,
				     const struct cf_work *w, uint32_t timer_slot)
{
	struct cf_witem *it;

	if (f->stopping) {
		f->st.work_refused_stopping++;
		return CF_E_STOPPING;
	}
	it = wq_pop(&f->w_free);
	if (!it) {
		f->st.work_dropped_nobuf++;
		return CF_E_NOBUF;
	}
	f->n_w_free--;
	it->w = *w;
	it->timer_slot = timer_slot;
	wq_push(&f->w_ready, it);
	f->st.work_posted++;
	f->ops.wake(f->ops.ctx);      /* under the lock: the cv contract */
	return CF_OK;
}

cf_status_t cf_post(struct vms_cluster_fork *f, const struct cf_work *w)
{
	cf_status_t st;

	if (!f || !w || (unsigned)w->owner >= (unsigned)CF_OWNER__COUNT)
		return CF_E_INVAL;

	f->ops.lock(f->ops.ctx);
	st = cf_enqueue_locked(f, w, 0);
	f->ops.unlock(f->ops.ctx);
	return st;
}

/* ==================================================================== *
 * 5a. The SERVED-I/O WORKER (FC-P6.6, design §3.2.6's E42 corollary)
 *
 * The one place in the cluster stack where a BLOCKING substrate call is legal,
 * and the queue discipline that gets a result from there back to the fork
 * thread. Read vms_cluster_fork.h §7a first: it carries the contract, the
 * reason the fork thread may never make the call itself, and the lock order.
 *
 * Nothing here interprets a request. `op`, `tag`, `arg0..3` and `ptr` pass
 * through untouched, and the callback's status word is delivered VERBATIM --
 * a served READ's answer on the wire is the block layer's real answer, never a
 * value this module chose (INV-6).
 * ==================================================================== */

/* Is a worker actually available for `owner`? Caller holds the queue lock.
 * "Available" means the binding gave us the three §15/§2 worker ops AND the
 * layer registered a callback -- either half missing is an honest refusal, not
 * a reason to run the I/O somewhere it does not belong. */
static int cf_io_available_locked(const struct vms_cluster_fork *f,
				  enum cf_owner owner)
{
	return f->ops.io_wait && f->ops.io_wake && f->ops.io_should_stop &&
	       f->io_cb[owner] != CF_NULL;
}

cf_status_t cf_set_io_handler(struct vms_cluster_fork *f, enum cf_owner owner,
			      cf_io_handler_t cb, void *ctx)
{
	if (!f || (unsigned)owner >= (unsigned)CF_OWNER__COUNT)
		return CF_E_INVAL;
	f->ops.lock(f->ops.ctx);
	f->io_cb[owner]  = cb;
	f->io_ctx[owner] = ctx;
	if (cb) {
		/* Registering re-opens the worker after a stop: a layer that
		 * stopped serving and started again is served again. */
		f->io_stopping = 0;
		f->st.io_stopping = 0;
	}
	f->ops.unlock(f->ops.ctx);
	return CF_OK;
}

/* Caller holds the queue lock. Take an io item AND the work item its completion
 * will use, or neither (see struct cf_ioitem's note). */
static struct cf_ioitem *cf_io_alloc_locked(struct vms_cluster_fork *f)
{
	struct cf_ioitem *it = ioq_pop(&f->io_free);

	if (!it)
		return CF_NULL;
	it->done = wq_pop(&f->w_free);
	if (!it->done) {
		ioq_push(&f->io_free, it);
		return CF_NULL;
	}
	f->n_io_free--;
	f->n_w_free--;
	return it;
}

/* Caller holds the queue lock. Give both halves back. `done` is released only
 * if it is still ours -- once the completion is queued it belongs to the work
 * queue. */
static void cf_io_release_locked(struct vms_cluster_fork *f,
				 struct cf_ioitem *it)
{
	if (it->done) {
		cf_zero(it->done, (uint32_t)sizeof(*it->done));
		wq_push(&f->w_free, it->done);
		f->n_w_free++;
	}
	cf_zero(it, (uint32_t)sizeof(*it));
	ioq_push(&f->io_free, it);
	f->n_io_free++;
}

cf_status_t cf_io_post(struct vms_cluster_fork *f, const struct cf_io *io)
{
	struct cf_ioitem *it;

	if (!f || !io || (unsigned)io->owner >= (unsigned)CF_OWNER__COUNT)
		return CF_E_INVAL;

	f->ops.lock(f->ops.ctx);
	if (!cf_io_available_locked(f, (enum cf_owner)io->owner)) {
		f->st.io_refused_noworker++;
		f->ops.unlock(f->ops.ctx);
		return CF_E_NOWORKER;
	}
	if (f->stopping || f->io_stopping) {
		f->st.io_refused_stopping++;
		f->ops.unlock(f->ops.ctx);
		return CF_E_STOPPING;
	}
	it = cf_io_alloc_locked(f);
	if (!it) {
		f->st.io_dropped_nobuf++;
		f->ops.unlock(f->ops.ctx);
		return CF_E_NOBUF;
	}
	it->io = *io;
	ioq_push(&f->io_ready, it);
	f->st.io_posted++;
	f->ops.io_wake(f->ops.ctx);   /* under the lock: the cv contract */
	f->ops.unlock(f->ops.ctx);
	return CF_OK;
}

void cf_io_start(struct vms_cluster_fork *f)
{
	if (!f)
		return;
	f->ops.lock(f->ops.ctx);
	f->io_stopping = 0;
	f->st.io_stopping = 0;
	f->ops.unlock(f->ops.ctx);
}

void cf_io_request_stop(struct vms_cluster_fork *f)
{
	if (!f)
		return;
	f->ops.lock(f->ops.ctx);
	f->io_stopping = 1;
	f->st.io_stopping = 1;
	if (f->ops.io_wake)
		f->ops.io_wake(f->ops.ctx);   /* under the lock: cv contract */
	f->ops.unlock(f->ops.ctx);
}

/* Caller holds the queue lock. Return every QUEUED request to the pool: the
 * layer that submitted them is going away, so running them would be work for
 * nobody. Counted, never silently discarded. */
static void cf_io_abandon_locked(struct vms_cluster_fork *f)
{
	struct cf_ioitem *it;

	while ((it = ioq_pop(&f->io_ready)) != CF_NULL) {
		cf_io_release_locked(f, it);
		f->st.io_abandoned++;
	}
}

/* Take the next request, or report that there is none. Returns 1 with the
 * request copied out and the owner's callback and its ctx bound; the pool entry
 * is already back on the free list, so a callback may submit while it runs. */
static int cf_io_take(struct vms_cluster_fork *f, struct cf_ioitem **out,
		      cf_io_handler_t *cb, void **cbctx)
{
	struct cf_ioitem *it;

	f->ops.lock(f->ops.ctx);
	if (f->io_stopping || f->stopping) {
		cf_io_abandon_locked(f);
		f->ops.unlock(f->ops.ctx);
		return 0;
	}
	it = ioq_pop(&f->io_ready);
	if (!it) {
		f->ops.unlock(f->ops.ctx);
		return 0;
	}
	/*
	 * The item stays OFF the free list for the whole callback: it carries
	 * the reserved completion work item, which is what guarantees this
	 * request can be answered.
	 */
	*out   = it;
	*cb    = f->io_cb[it->io.owner];
	*cbctx = f->io_ctx[it->io.owner];
	f->st.io_started++;
	f->ops.unlock(f->ops.ctx);
	return 1;
}

/*
 * Hand one finished request's status back to its layer as an ordinary work
 * item, in the item RESERVED for it at submit time -- so an accepted request is
 * always answered. The only thing that can still stop the answer is a stop
 * (which is tearing the layer down anyway), and that is counted.
 */
static void cf_io_complete(struct vms_cluster_fork *f, struct cf_ioitem *it,
			   uint32_t status)
{
	struct cf_witem *done;

	f->ops.lock(f->ops.ctx);
	done = it->done;
	if (f->stopping) {
		f->st.io_completion_dropped++;
	} else {
		it->done = CF_NULL;   /* it belongs to the work queue now */
		done->w.owner = it->io.owner;
		done->w.kind  = CF_WORK_IO_DONE;
		done->w.arg0  = it->io.tag;
		done->w.arg1  = status;
		done->w.ptr   = CF_NULL;
		done->timer_slot = 0;
		wq_push(&f->w_ready, done);
		f->st.work_posted++;
		f->st.io_completed++;
		f->ops.wake(f->ops.ctx);   /* under the lock: the cv contract */
	}
	cf_io_release_locked(f, it);
	/* The worker just released a request: a stop waiting for it to go idle
	 * is woken here, under the lock, by the cv contract. */
	if (f->ops.io_wake)
		f->ops.io_wake(f->ops.ctx);
	f->ops.unlock(f->ops.ctx);
}

int cf_io_run_one(struct vms_cluster_fork *f)
{
	struct cf_ioitem *it = CF_NULL;
	cf_io_handler_t cb = CF_NULL;
	void *cbctx = CF_NULL;
	struct cf_io io;
	uint32_t status;

	if (!f)
		return 0;
	if (!cf_io_take(f, &it, &cb, &cbctx))
		return 0;
	if (!cb) {
		/* Unregistered between the post and the pop. There is nobody to
		 * run it; the completion still goes back so the submitting
		 * layer's request cannot hang. */
		cf_io_complete(f, it, (uint32_t)CF_E_NOWORKER);
		return 1;
	}

	/*
	 * THE BLOCKING CALL, and the only one in the stack: no queue lock, no
	 * fork mutex, no protocol state -- just the layer's callback and the
	 * disk. The request is COPIED out first, because the callback may run
	 * for tens of milliseconds and must not be reading a pool entry.
	 */
	io = it->io;
	status = cb(cbctx, &io);
	cf_io_complete(f, it, status);
	return 1;
}

/* Sleep until there is a request or the worker must exit. Same predicate-first
 * shape as cf_wait_ready, and the same reason: a stop must not lose an event
 * that is already queued -- except that here a stop DISCARDS the queue (see
 * cf_io_request_stop), because its owner is being torn down. */
static int cf_io_wait_ready(struct vms_cluster_fork *f)
{
	int ready;

	f->ops.lock(f->ops.ctx);
	for (;;) {
		if (f->io_stopping || f->stopping ||
		    f->ops.io_should_stop(f->ops.ctx)) {
			ready = 0;
			break;
		}
		if (f->io_ready.head) {
			ready = 1;
			break;
		}
		f->st.io_waits++;
		(void)f->ops.io_wait(f->ops.ctx);
	}
	f->ops.unlock(f->ops.ctx);
	return ready;
}

void cf_io_run(struct vms_cluster_fork *f)
{
	if (!f)
		return;
	if (!f->ops.io_wait || !f->ops.io_wake || !f->ops.io_should_stop)
		return;   /* no worker ops: the thread has nothing to run */
	while (cf_io_wait_ready(f))
		(void)cf_io_run_one(f);

	/* Leaving: anything still queued belongs to a layer that is going away.
	 * Return it to the pool and count it, so the pool is whole for a
	 * restart and nothing is lost silently. */
	f->ops.lock(f->ops.ctx);
	cf_io_abandon_locked(f);
	f->ops.unlock(f->ops.ctx);
}

/* ==================================================================== *
 * 6. Timers -- CONTRACT RULE 2 lives here, once, for the whole stack
 * ==================================================================== */

/* Caller holds the queue lock. */
static struct cf_tslot *cf_slot_find(struct vms_cluster_fork *f,
				     enum cf_owner owner, uint32_t which,
				     uint32_t key)
{
	uint32_t i;

	for (i = 0; i < f->cfg.timer_slots; i++) {
		struct cf_tslot *s = &f->tslots[i];

		if (s->in_use && s->owner == (uint16_t)owner &&
		    s->which == which && s->key == key)
			return s;
	}
	return CF_NULL;
}

/* Caller holds the queue lock. */
static struct cf_tslot *cf_slot_alloc(struct vms_cluster_fork *f,
				      enum cf_owner owner, uint32_t which,
				      uint32_t key)
{
	uint32_t i;

	for (i = 0; i < f->cfg.timer_slots; i++) {
		struct cf_tslot *s = &f->tslots[i];

		if (s->in_use)
			continue;
		s->in_use  = 1;
		s->armed   = 0;
		s->pending = 0;
		s->owner   = (uint16_t)owner;
		s->which   = which;
		s->key     = key;
		f->n_slots++;
		return s;
	}
	return CF_NULL;
}

cf_status_t cf_timer_arm(struct vms_cluster_fork *f, enum cf_owner owner,
			 uint32_t which, uint32_t key, uint32_t ms)
{
	struct cf_tslot *s;
	uint32_t idx;

	if (!f || (unsigned)owner >= (unsigned)CF_OWNER__COUNT)
		return CF_E_INVAL;

	f->ops.lock(f->ops.ctx);
	s = cf_slot_find(f, owner, which, key);
	if (!s)
		s = cf_slot_alloc(f, owner, which, key);
	if (!s) {
		f->ops.unlock(f->ops.ctx);
		return CF_E_NOSLOT;   /* honest: never a silently unarmed timer */
	}
	s->armed = 1;
	idx = (uint32_t)(s - f->tslots);
	f->st.timer_arms++;
	/* Armed UNDER the queue lock on purpose: ops->timer_arm does not sleep
	 * (mod_timer / callout_schedule), and doing it here makes "the slot is
	 * armed" and "the substrate timer is running" one atomic fact, so a
	 * concurrent expiry callback can never see them disagree. */
	f->ops.timer_arm(f->ops.ctx, idx, ms);
	f->ops.unlock(f->ops.ctx);
	return CF_OK;
}

/* Caller holds the queue lock. Pull any queued expiry for `idx` back off the
 * work queue and return it to the free pool, so a cancelled timer cannot be
 * dispatched against an object the caller is about to free. */
static void cf_purge_expiry_locked(struct vms_cluster_fork *f, uint32_t idx)
{
	struct cf_wq keep;
	struct cf_witem *it;

	keep.head = CF_NULL;
	keep.tail = CF_NULL;

	while ((it = wq_pop(&f->w_ready)) != CF_NULL) {
		if (it->timer_slot == idx + 1) {
			cf_zero(it, (uint32_t)sizeof(*it));
			wq_push(&f->w_free, it);
			f->n_w_free++;
			f->st.timer_dequeued++;
		} else {
			wq_push(&keep, it);
		}
	}
	f->w_ready = keep;
}

void cf_timer_cancel(struct vms_cluster_fork *f, enum cf_owner owner,
		     uint32_t which, uint32_t key)
{
	struct cf_tslot *s;
	uint32_t idx;

	if (!f || (unsigned)owner >= (unsigned)CF_OWNER__COUNT)
		return;

	f->ops.lock(f->ops.ctx);
	s = cf_slot_find(f, owner, which, key);
	if (!s) {
		f->ops.unlock(f->ops.ctx);
		return;
	}
	idx = (uint32_t)(s - f->tslots);
	s->armed = 0;   /* a callback already in flight now finds it disarmed */
	f->ops.unlock(f->ops.ctx);

	/* MAY SLEEP, and MUST NOT run under the queue lock: it waits out a
	 * callback that is itself trying to take that lock (del_timer_sync /
	 * callout_halt). After it returns, no callback for this slot is
	 * running or can start. */
	f->ops.timer_cancel(f->ops.ctx, idx);

	f->ops.lock(f->ops.ctx);
	cf_purge_expiry_locked(f, idx);
	s->pending = 0;
	s->in_use  = 0;             /* released only now -- the callback is out */
	if (f->n_slots)
		f->n_slots--;
	f->st.timer_cancels++;
	f->ops.unlock(f->ops.ctx);
}

/*
 * The substrate timer callback's ONLY entry point: post a work item and wake.
 * Coalescing lives here -- while an expiry for this slot is queued and not yet
 * dispatched, a further expiry adds nothing but a duplicate event, so it is
 * counted and dropped. (VMS's own analogue: a fork block that is already on the
 * queue is not queued twice.)
 */
void cf_timer_expired(struct vms_cluster_fork *f, uint32_t slot)
{
	struct cf_tslot *s;
	struct cf_work w;

	if (!f)
		return;

	f->ops.lock(f->ops.ctx);
	if (slot >= f->cfg.timer_slots) {
		f->st.timer_stale++;
		f->ops.unlock(f->ops.ctx);
		return;
	}
	s = &f->tslots[slot];
	if (!s->in_use || !s->armed) {
		f->st.timer_stale++;      /* cancelled or released under us */
		f->ops.unlock(f->ops.ctx);
		return;
	}
	s->armed = 0;   /* exec_timer fires ONCE; a cadence is a re-arm */

	if (s->pending) {
		/* An expiry for this identity is already queued and not yet
		 * dispatched -- a second one carries no information the first
		 * does not, so it is counted and dropped rather than queued
		 * twice. (This is reachable exactly when something re-armed the
		 * identity before the fork thread got to the first expiry.) */
		f->st.timer_coalesced++;
		f->ops.unlock(f->ops.ctx);
		return;
	}

	w.owner = s->owner;
	w.kind  = CF_WORK_TIMER;
	w.arg0  = s->which;
	w.arg1  = s->key;
	w.ptr   = CF_NULL;

	if (cf_enqueue_locked(f, &w, slot + 1) == CF_OK) {
		s->pending = 1;
		f->st.timer_expiries++;
	} else {
		/* The work pool was empty, or a stop is in progress. The tick
		 * is LOST -- counted, never faked -- and the layer will notice
		 * through its own deadline, exactly as it must when a frame is
		 * dropped. */
		f->st.timer_dropped_nobuf++;
	}
	f->ops.unlock(f->ops.ctx);
}

/* ==================================================================== *
 * 7. Dispatch -- one event at a time, under the fork mutex
 * ==================================================================== */

/*
 * Which queue to serve. Both are FIFO ("queue priority is based on time spent
 * in the queue", Davis pp. 2-45/2-46), and when both have work the module
 * ALTERNATES. That is a deliberate choice over "frames first": a burst of
 * received frames -- exactly what a cluster rebuild is -- must not starve the
 * work queue, because a TIMVCFAIL or RECNXINTERVAL expiry sits in it and a
 * starved deadline is a false circuit failure. The converse (work re-posting
 * work) must not starve the receive queue either. Alternation is bounded both
 * ways and is deterministic, so a test can assert the order.
 *
 * Caller holds the queue lock. Returns 1 for "serve rx", 0 for "serve work",
 * -1 for "both empty".
 */
static int cf_pick_queue(struct vms_cluster_fork *f)
{
	int have_rx = f->rx_ready.head != CF_NULL;
	int have_wk = f->w_ready.head != CF_NULL;

	if (have_rx && have_wk)
		return f->serve_rx_next ? 1 : 0;
	if (have_rx)
		return 1;
	if (have_wk)
		return 0;
	return -1;
}

/*
 * Take the next event off the queues. On return either *rb_out is a receive
 * buffer the caller must release with cf_release_rxbuf, or *w_out holds a copy
 * of a work item (its pool entry is already back on the free list, so a handler
 * may post while it runs). Returns 1 if an event was taken.
 */
static int cf_take_event(struct vms_cluster_fork *f, struct cf_rxbuf **rb_out,
			 struct cf_work *w_out)
{
	struct cf_witem *it;
	int pick;

	*rb_out = CF_NULL;

	f->ops.lock(f->ops.ctx);
	pick = cf_pick_queue(f);
	if (pick < 0) {
		f->ops.unlock(f->ops.ctx);
		return 0;
	}
	if (pick == 1) {
		*rb_out = rxq_pop(&f->rx_ready);
		f->serve_rx_next = 0;
		f->ops.unlock(f->ops.ctx);
		return 1;
	}

	it = wq_pop(&f->w_ready);
	*w_out = it->w;
	if (it->timer_slot && it->timer_slot - 1 < f->cfg.timer_slots)
		f->tslots[it->timer_slot - 1].pending = 0;
	cf_zero(it, (uint32_t)sizeof(*it));
	wq_push(&f->w_free, it);
	f->n_w_free++;
	f->serve_rx_next = 1;
	f->ops.unlock(f->ops.ctx);
	return 1;
}

static void cf_release_rxbuf(struct vms_cluster_fork *f, struct cf_rxbuf *b)
{
	f->ops.lock(f->ops.ctx);
	b->b.len = 0;
	rxq_push(&f->rx_free, b);
	f->n_rx_free++;
	f->ops.unlock(f->ops.ctx);
}

/* Runs under the fork mutex. */
static void cf_deliver_rx(struct vms_cluster_fork *f, struct cf_rxbuf *b)
{
	if (!f->rx_cb) {
		f->st.rx_undeliverable++;
		return;
	}
	f->rx_cb(f->rx_ctx, b->b.data, b->b.len);
	f->st.rx_dispatched++;
}

/* Runs under the fork mutex. */
static void cf_deliver_work(struct vms_cluster_fork *f, const struct cf_work *w)
{
	cf_work_handler_t cb;

	if ((unsigned)w->owner >= (unsigned)CF_OWNER__COUNT ||
	    !f->work_cb[w->owner]) {
		f->st.work_undeliverable++;
		return;
	}
	cb = f->work_cb[w->owner];
	cb(f->work_ctx[w->owner], w);
	f->st.work_dispatched++;
}

int cf_dispatch_one(struct vms_cluster_fork *f)
{
	struct cf_rxbuf *rb;
	struct cf_work w;

	if (!f)
		return 0;
	if (!cf_take_event(f, &rb, &w))
		return 0;

	f->ops.fork_lock(f->ops.ctx);
	if (rb)
		cf_deliver_rx(f, rb);
	else
		cf_deliver_work(f, &w);
	f->ops.fork_unlock(f->ops.ctx);

	if (rb)
		cf_release_rxbuf(f, rb);
	return 1;
}

/* ==================================================================== *
 * 8. The thread body
 * ==================================================================== */

/*
 * Sleep until there is an event, or until the context must exit. Returns 1 when
 * an event is queued, 0 when the thread should return from cf_run.
 *
 * The QUEUE-NONEMPTY test comes FIRST, before the stop test: that is the whole
 * of the "stop while work is pending drains cleanly" rule. A stop refuses NEW
 * work (cf_enqueue_locked / cf_rx_deliver), so the queues can only shrink from
 * that moment and the drain terminates; but a last-gasp or teardown item posted
 * before the stop still runs. The predicate also has priority over an
 * interrupted wait, which is exec_kbackend.h §2's rule and VMS's: a wait ends
 * when its condition is satisfied.
 */
static int cf_wait_ready(struct vms_cluster_fork *f)
{
	int ready;

	f->ops.lock(f->ops.ctx);
	for (;;) {
		if (f->rx_ready.head || f->w_ready.head) {
			ready = 1;
			break;
		}
		if (f->stopping || f->ops.should_stop(f->ops.ctx)) {
			ready = 0;
			break;
		}
		f->st.waits++;
		if (f->ops.wait(f->ops.ctx))
			f->st.wait_interrupts++;
	}
	f->ops.unlock(f->ops.ctx);
	return ready;
}

void cf_run(struct vms_cluster_fork *f)
{
	if (!f)
		return;
	while (cf_wait_ready(f))
		(void)cf_dispatch_one(f);
}

void cf_request_stop(struct vms_cluster_fork *f)
{
	if (!f)
		return;
	f->ops.lock(f->ops.ctx);
	f->stopping = 1;
	f->st.stopping = 1;
	f->ops.wake(f->ops.ctx);      /* under the lock: the cv contract */
	/* The SERVED-I/O WORKER sleeps on its OWN cv, so the context's stop has
	 * to wake that one too or the worker would sleep through the teardown
	 * its own layer is being torn down by. */
	if (f->ops.io_wake)
		f->ops.io_wake(f->ops.ctx);
	f->ops.unlock(f->ops.ctx);
}

void *cf_ops_ctx(struct vms_cluster_fork *f)
{
	return f ? f->ops.ctx : (void *)0;
}

void cf_stats_get(struct vms_cluster_fork *f, struct cf_stats *out)
{
	if (!f || !out)
		return;
	f->ops.lock(f->ops.ctx);
	*out = f->st;
	out->rx_free          = f->n_rx_free;
	out->work_free        = f->n_w_free;
	out->io_free          = f->n_io_free;
	out->timer_slots_used = f->n_slots;
	f->ops.unlock(f->ops.ctx);
}
