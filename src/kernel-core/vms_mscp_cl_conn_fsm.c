/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_mscp_cl_conn_fsm.c - the MSCP disk class driver's connect-admission FSM
 * (E64: lookup before connect, and the join drives alone).
 *
 * The contract, the two rules and the grounding for each are in
 * vms_mscp_cl_conn_fsm.h. This file is the [state][event] table and its
 * handlers, and it decides nothing that file does not state.
 *
 * PURE TU (CI gate rule 4): no seam primitive, no allocation, no clock but
 * ops->now_ms.
 */

#include "vms_mscp_cl_conn_fsm.h"

/* ==========================================================================
 * Time: wrap-safe, injected (the vms_cnxman_recnx_fsm.c form)
 * ========================================================================== */

static int conn_reached(uint32_t now, uint32_t deadline)
{
	return (int32_t)(now - deadline) >= 0;
}

static uint32_t conn_now(const struct mscp_cl_conn *c)
{
	if (c->ops != (const struct mscp_cl_conn_ops *)0 &&
	    c->ops->now_ms != (uint32_t (*)(void *))0)
		return c->ops->now_ms(c->ops->ctx);
	return 0u;
}

static void conn_log(struct mscp_cl_conn *c, const char *msg)
{
	if (c->ops != (const struct mscp_cl_conn_ops *)0 &&
	    c->ops->log != (void (*)(void *, const char *))0)
		c->ops->log(c->ops->ctx, msg);
}

/* Enter a state and stamp WHEN, because every wait in this FSM is measured
 * from the moment the state was entered and never from a free-running tick. */
static void conn_goto(struct mscp_cl_conn *c, struct mscp_cl_conn_peer *p,
		      enum mscp_cl_conn_state s, uint32_t now)
{
	(void)c;
	p->state = (uint8_t)s;
	p->since_ms = now;
}

/* ==========================================================================
 * The peer table
 * ========================================================================== */

struct mscp_cl_conn_peer *mscp_cl_conn_by_sysid(struct mscp_cl_conn *c,
						vms_scs_sysid_t sysid)
{
	uint32_t i;

	if (c == (struct mscp_cl_conn *)0 || sysid == 0u)
		return (struct mscp_cl_conn_peer *)0;
	for (i = 0; i < c->n_peers; i++) {
		if (c->peers[i].in_use && c->peers[i].sysid == sysid)
			return &c->peers[i];
	}
	return (struct mscp_cl_conn_peer *)0;
}

struct mscp_cl_conn_peer *mscp_cl_conn_by_conid(struct mscp_cl_conn *c,
						vms_conid_t conid)
{
	uint32_t i;

	if (c == (struct mscp_cl_conn *)0 || conid == 0u)
		return (struct mscp_cl_conn_peer *)0;
	for (i = 0; i < c->n_peers; i++) {
		if (c->peers[i].in_use && c->peers[i].conid == conid)
			return &c->peers[i];
	}
	return (struct mscp_cl_conn_peer *)0;
}

static struct mscp_cl_conn_peer *conn_alloc(struct mscp_cl_conn *c,
					     vms_scs_sysid_t sysid,
					     uint32_t now)
{
	uint32_t i;

	for (i = 0; i < c->n_peers; i++) {
		struct mscp_cl_conn_peer *p = &c->peers[i];

		if (p->in_use)
			continue;
		p->in_use = 1u;
		p->state = (uint8_t)MSCP_CL_CONN_IDLE;
		p->spoken = 0u;   /* never asked: due on THIS sweep */
		p->pad0 = 0u;
		p->sysid = sysid;
		p->conid = 0u;
		p->since_ms = now;
		return p;
	}
	c->no_peer_slot++;
	return (struct mscp_cl_conn_peer *)0;
}

/* ==========================================================================
 * The handlers. One event, one fact, one edge.
 * ========================================================================== */

typedef void (*conn_handler_t)(struct mscp_cl_conn *c,
			       struct mscp_cl_conn_peer *p, uint32_t now);

/*
 * IDLE + SWEEP: ask, once the retry period since the last thing that happened
 * to this member has passed. Asking IS opening our own transient
 * SCS$DIRECTORY round (p. 2-51), which is why it is gated at all.
 */
static void h_idle_sweep(struct mscp_cl_conn *c, struct mscp_cl_conn_peer *p,
			 uint32_t now)
{
	int rc;

	if (p->spoken &&
	    !conn_reached(now, p->since_ms + MSCP_CL_CONN_RETRY_MS))
		return;
	if (c->ops == (const struct mscp_cl_conn_ops *)0 ||
	    c->ops->dir_inquire == (int (*)(void *, vms_scs_sysid_t,
					    const uint8_t *))0 ||
	    c->name_mscp_disk == (const uint8_t *)0) {
		c->inquiry_failures++;
		return;
	}
	rc = c->ops->dir_inquire(c->ops->ctx, p->sysid, c->name_mscp_disk);
	p->spoken = 1u;
	if (rc != 0) {
		c->inquiry_failures++;
		p->since_ms = now;   /* do not hammer the directory */
		return;
	}
	c->inquiries++;
	conn_goto(c, p, MSCP_CL_CONN_ASKING, now);
}

/*
 * ASKING + SWEEP: silence is not an answer. An inquiry nobody answered goes
 * back to IDLE, is COUNTED, and is asked again -- never recorded as absence.
 */
static void h_asking_sweep(struct mscp_cl_conn *c, struct mscp_cl_conn_peer *p,
			   uint32_t now)
{
	if (!conn_reached(now, p->since_ms + MSCP_CL_CONN_ASK_TIMEOUT_MS))
		return;
	c->unanswered++;
	conn_goto(c, p, MSCP_CL_CONN_IDLE, now);
}

/* PRESENT + SWEEP: the member really said yes, so connect. */
static void h_present_sweep(struct mscp_cl_conn *c,
			    struct mscp_cl_conn_peer *p, uint32_t now)
{
	vms_conid_t conid = 0u;

	if (c->ops == (const struct mscp_cl_conn_ops *)0 ||
	    c->ops->connect == (int (*)(void *, vms_scs_sysid_t,
					vms_conid_t *))0) {
		c->connect_refusals++;
		conn_goto(c, p, MSCP_CL_CONN_IDLE, now);
		return;
	}
	if (c->ops->connect(c->ops->ctx, p->sysid, &conid) != 0 ||
	    conid == 0u) {
		c->connect_refusals++;
		conn_goto(c, p, MSCP_CL_CONN_IDLE, now);
		return;
	}
	p->conid = conid;
	c->connects++;
	conn_goto(c, p, MSCP_CL_CONN_CONNECTING, now);
}

/*
 * ABSENT + SWEEP: the member answered "NOT PRESENT HERE" -- a real
 * configuration (MSCP_LOAD 0), not a failure. It is asked again after the
 * retry period, because it may mount its first served volume later.
 */
static void h_absent_sweep(struct mscp_cl_conn *c, struct mscp_cl_conn_peer *p,
			   uint32_t now)
{
	if (!conn_reached(now, p->since_ms + MSCP_CL_CONN_RETRY_MS))
		return;
	p->spoken = 0u;   /* the retry period is over: due on the next beat */
	conn_goto(c, p, MSCP_CL_CONN_IDLE, now);
}

static void h_hit(struct mscp_cl_conn *c, struct mscp_cl_conn_peer *p,
		  uint32_t now)
{
	c->hits++;
	conn_goto(c, p, MSCP_CL_CONN_PRESENT, now);
}

static void h_miss(struct mscp_cl_conn *c, struct mscp_cl_conn_peer *p,
		   uint32_t now)
{
	c->misses++;
	conn_goto(c, p, MSCP_CL_CONN_ABSENT, now);
	conn_log(c, "%MSCP_CL, the member answered NOT PRESENT HERE for "
		    "MSCP$DISK: it serves no disks");
}

/* CONNECTING / OPEN + SWEEP: this member's leg is in SCS's hands; the beat has
 * nothing to do for it. An explicit edge, so a connected member does not count
 * as an ignored event once a second for the life of the cluster. */
static void h_nothing_to_do(struct mscp_cl_conn *c,
			    struct mscp_cl_conn_peer *p, uint32_t now)
{
	(void)c; (void)p; (void)now;
}

static void h_opened(struct mscp_cl_conn *c, struct mscp_cl_conn_peer *p,
		     uint32_t now)
{
	c->opens++;
	conn_goto(c, p, MSCP_CL_CONN_OPEN, now);
}

/*
 * Back to IDLE on either close: the name is re-resolved before the next
 * attempt, because a member that closed on us may have stopped serving, and a
 * stale HIT is an assertion about a peer's present state that this node no
 * longer holds. `spoken` stays set, so the re-ask waits out the retry period.
 */
static void h_refused(struct mscp_cl_conn *c, struct mscp_cl_conn_peer *p,
		      uint32_t now)
{
	/* Closed before it ever opened: the member would not take our
	 * connection. A REAL refusal, distinct from a served connection later
	 * going away. */
	c->connect_refusals++;
	p->conid = 0u;
	conn_goto(c, p, MSCP_CL_CONN_IDLE, now);
}

static void h_closed(struct mscp_cl_conn *c, struct mscp_cl_conn_peer *p,
		     uint32_t now)
{
	c->closes++;
	p->conid = 0u;
	conn_goto(c, p, MSCP_CL_CONN_IDLE, now);
}

static const conn_handler_t
conn_table[MSCP_CL_CONN_STATE__COUNT][MSCP_CL_CONN_EV__COUNT] = {
	[MSCP_CL_CONN_IDLE] = {
		[MSCP_CL_CONN_EV_SWEEP]  = h_idle_sweep,
	},
	[MSCP_CL_CONN_ASKING] = {
		[MSCP_CL_CONN_EV_SWEEP]  = h_asking_sweep,
		[MSCP_CL_CONN_EV_HIT]    = h_hit,
		[MSCP_CL_CONN_EV_MISS]   = h_miss,
	},
	[MSCP_CL_CONN_PRESENT] = {
		[MSCP_CL_CONN_EV_SWEEP]  = h_present_sweep,
		[MSCP_CL_CONN_EV_MISS]   = h_miss,
	},
	[MSCP_CL_CONN_ABSENT] = {
		[MSCP_CL_CONN_EV_SWEEP]  = h_absent_sweep,
		[MSCP_CL_CONN_EV_HIT]    = h_hit,
	},
	[MSCP_CL_CONN_CONNECTING] = {
		[MSCP_CL_CONN_EV_SWEEP]  = h_nothing_to_do,
		[MSCP_CL_CONN_EV_OPENED] = h_opened,
		[MSCP_CL_CONN_EV_CLOSED] = h_refused,
	},
	[MSCP_CL_CONN_OPEN] = {
		[MSCP_CL_CONN_EV_SWEEP]  = h_nothing_to_do,
		[MSCP_CL_CONN_EV_CLOSED] = h_closed,
	},
};

static void conn_dispatch(struct mscp_cl_conn *c, struct mscp_cl_conn_peer *p,
			  enum mscp_cl_conn_event ev, uint32_t now)
{
	conn_handler_t h;

	if ((unsigned)ev >= (unsigned)MSCP_CL_CONN_EV__COUNT ||
	    (unsigned)p->state >= (unsigned)MSCP_CL_CONN_STATE__COUNT) {
		c->ignored_events++;
		return;
	}
	h = conn_table[p->state][ev];
	if (h == (conn_handler_t)0) {
		c->ignored_events++;
		return;
	}
	h(c, p, now);
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

static void conn_zero(struct mscp_cl_conn *c)
{
	uint8_t *b = (uint8_t *)c;
	uint32_t i;

	for (i = 0; i < (uint32_t)sizeof(*c); i++)
		b[i] = 0u;
}

int mscp_cl_conn_init(struct mscp_cl_conn *c,
		      const struct mscp_cl_conn_ops *ops,
		      const uint8_t *name_mscp_disk)
{
	if (c == (struct mscp_cl_conn *)0 ||
	    ops == (const struct mscp_cl_conn_ops *)0 ||
	    name_mscp_disk == (const uint8_t *)0)
		return -1;
	conn_zero(c);
	c->ops = ops;
	c->name_mscp_disk = name_mscp_disk;
	return 0;
}

int mscp_cl_conn_bind_peers(struct mscp_cl_conn *c,
			    struct mscp_cl_conn_peer *p, uint32_t n)
{
	uint32_t i;

	if (c == (struct mscp_cl_conn *)0 ||
	    p == (struct mscp_cl_conn_peer *)0 || n == 0u)
		return -1;
	for (i = 0; i < n; i++) {
		p[i].in_use = 0u;
		p[i].state = (uint8_t)MSCP_CL_CONN_IDLE;
		p[i].spoken = 0u;
		p[i].pad0 = 0u;
		p[i].sysid = 0u;
		p[i].conid = 0u;
		p[i].since_ms = 0u;
	}
	c->peers = p;
	c->n_peers = n;
	return 0;
}

/* ==========================================================================
 * The beat
 * ========================================================================== */

static int conn_in_list(const vms_scs_sysid_t *sysids, uint32_t n,
			vms_scs_sysid_t sysid)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if (sysids[i] == sysid)
			return 1;
	}
	return 0;
}

/* A member the port no longer has a circuit to keeps no row: its Con.ID is
 * gone with the circuit, and a row that outlives the circuit is a claim about
 * a system this node can no longer reach. */
static void conn_reap(struct mscp_cl_conn *c, const vms_scs_sysid_t *sysids,
		      uint32_t n)
{
	uint32_t i;

	for (i = 0; i < c->n_peers; i++) {
		struct mscp_cl_conn_peer *p = &c->peers[i];

		if (!p->in_use || conn_in_list(sysids, n, p->sysid))
			continue;
		p->in_use = 0u;
		p->state = (uint8_t)MSCP_CL_CONN_IDLE;
		p->spoken = 0u;
		p->sysid = 0u;
		p->conid = 0u;
		p->since_ms = 0u;
	}
}

/* Rule 2: while this node is still joining, the join FSM is the sole
 * originator on the channel. */
static int conn_joined(struct mscp_cl_conn *c)
{
	if (c->ops == (const struct mscp_cl_conn_ops *)0 ||
	    c->ops->joined == (int (*)(void *))0)
		return 0;
	return c->ops->joined(c->ops->ctx) != 0;
}

static int conn_join_holds(struct mscp_cl_conn *c, vms_scs_sysid_t sysid)
{
	if (c->ops == (const struct mscp_cl_conn_ops *)0 ||
	    c->ops->join_holds == (int (*)(void *, vms_scs_sysid_t))0)
		return 0;
	return c->ops->join_holds(c->ops->ctx, sysid) != 0;
}

uint32_t mscp_cl_conn_sweep(struct mscp_cl_conn *c,
			    const vms_scs_sysid_t *sysids, uint32_t n)
{
	uint32_t i, swept = 0u, now;

	if (c == (struct mscp_cl_conn *)0 ||
	    c->peers == (struct mscp_cl_conn_peer *)0)
		return 0u;
	if (sysids == (const vms_scs_sysid_t *)0)
		n = 0u;

	c->sweeps++;
	now = conn_now(c);
	conn_reap(c, sysids, n);

	if (!conn_joined(c)) {
		c->deferred_joining += n;
		return 0u;
	}

	for (i = 0; i < n; i++) {
		struct mscp_cl_conn_peer *p;

		if (sysids[i] == 0u)
			continue;
		if (conn_join_holds(c, sysids[i])) {
			c->deferred_join_owned++;
			continue;
		}
		p = mscp_cl_conn_by_sysid(c, sysids[i]);
		if (p == (struct mscp_cl_conn_peer *)0) {
			p = conn_alloc(c, sysids[i], now);
			if (p == (struct mscp_cl_conn_peer *)0)
				continue;
		}
		conn_dispatch(c, p, MSCP_CL_CONN_EV_SWEEP, now);
		swept++;
	}
	return swept;
}

/* ==========================================================================
 * Answers and connection events
 * ========================================================================== */

static int conn_name_eq(const uint8_t *a, const uint8_t *b)
{
	uint32_t i;

	if (a == (const uint8_t *)0 || b == (const uint8_t *)0)
		return 0;
	for (i = 0; i < (uint32_t)VMS_SCS_PROCNAME_LEN; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}

void mscp_cl_conn_dir_result(struct mscp_cl_conn *c, vms_scs_sysid_t dst,
			     const uint8_t *name, int present)
{
	struct mscp_cl_conn_peer *p;

	if (c == (struct mscp_cl_conn *)0)
		return;
	/* An answer about a name this sweep never asked about advances
	 * nothing -- it is counted and dropped. */
	if (!conn_name_eq(name, c->name_mscp_disk)) {
		c->ignored_events++;
		return;
	}
	p = mscp_cl_conn_by_sysid(c, dst);
	if (p == (struct mscp_cl_conn_peer *)0) {
		c->ignored_events++;
		return;
	}
	conn_dispatch(c, p, present ? MSCP_CL_CONN_EV_HIT
				    : MSCP_CL_CONN_EV_MISS, conn_now(c));
}

void mscp_cl_conn_opened(struct mscp_cl_conn *c, vms_conid_t conid)
{
	struct mscp_cl_conn_peer *p;

	if (c == (struct mscp_cl_conn *)0)
		return;
	p = mscp_cl_conn_by_conid(c, conid);
	if (p == (struct mscp_cl_conn_peer *)0) {
		c->ignored_events++;
		return;
	}
	conn_dispatch(c, p, MSCP_CL_CONN_EV_OPENED, conn_now(c));
}

void mscp_cl_conn_closed(struct mscp_cl_conn *c, vms_conid_t conid)
{
	struct mscp_cl_conn_peer *p;

	if (c == (struct mscp_cl_conn *)0)
		return;
	p = mscp_cl_conn_by_conid(c, conid);
	if (p == (struct mscp_cl_conn_peer *)0) {
		c->ignored_events++;
		return;
	}
	conn_dispatch(c, p, MSCP_CL_CONN_EV_CLOSED, conn_now(c));
}

/* ==========================================================================
 * Names
 * ========================================================================== */

static const char *const conn_state_names[MSCP_CL_CONN_STATE__COUNT] = {
	"IDLE", "ASKING", "PRESENT", "ABSENT", "CONNECTING", "OPEN"
};

const char *mscp_cl_conn_state_name(enum mscp_cl_conn_state s)
{
	if ((unsigned)s >= (unsigned)MSCP_CL_CONN_STATE__COUNT)
		return "?";
	return conn_state_names[s];
}
