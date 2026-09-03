/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_mscp_cl.c - the MSCP disk class driver's executive glue (FC-P7.1).
 *
 * The contract, the connect policy, the served-device row and the one thing
 * this layer deliberately does not do (the synchronous ACP block seam) are all
 * in vms_mscp_cl.h. The driver ITSELF -- CDDB/UCB/CDRP, the
 * [state][event] table and every decision it makes -- is the pure
 * `struct mscp_cl_fsm` in vms_mscp_cl_io_fsm.{c,h}. This file owns storage and
 * bindings and decides nothing about the protocol.
 *
 * WHAT THIS FILE DOES NOT DO. It builds no frame and decodes none (the codec
 * owns that); it makes no MSCP decision (the driver FSM owns that); it composes
 * no device name (the pure TU does, from values this file READ out of the CSB);
 * and it reaches the substrate only through exec_kbackend.h and the FC-P0.5
 * fork API.
 *
 * INCLUDES: this TU is on the cluster core list enforced by
 * tools/ci/cluster_core_includes_gate.sh -- exec_kbackend.h and kernel-core
 * headers only, never a substrate header.
 */

#include "vms_internal.h"      /* the SS$_ vocabulary + vms_devtab_*_served_disk */
#include "exec_kbackend.h"
#include "vms_cluster.h"
#include "vms_cluster_fork.h"  /* FC-P0.5: cf_timer_*, cf_set_work_handler */
#include "vms_pe.h"            /* the port's THIRD service (block transfer) */
#include "vms_pe_fsm.h"        /* PE_BLK_ACC_* -- the port's own access bits */
#include "vms_scs.h"
#include "vms_cnxman.h"        /* SS5b: the ONE `VMS$DISK_CL_DRVR` registration */
#include "vms_cnxman_csb.h"    /* the CSB's FORK-CONTEXT accessor (see below) */
#include "vms_mscp_cl_io_fsm.h"
#include "vms_mscp_cl.h"

/* ==========================================================================
 * 0. Cadence -- OVMX design values, each labelled as one
 * ========================================================================== */

/*
 * The driver's beat: sweep for members to open an `MSCP$DISK` connection to,
 * and reap a request whose answer never came. One second -- the same cadence
 * vms_mscp_srv.c's own beat is sized at, and fast enough that a member that
 * starts serving is found within a beat.
 */
#define MSCP_CL_TIMER_BEAT 0u
#define MSCP_CL_BEAT_MS    1000u

/*
 * How long after a refused or closed `MSCP$DISK` connect this layer waits
 * before asking that member again. A member with MSCP_LOAD=0 will refuse
 * forever and must not be asked once a second; a member that mounts its first
 * volume later must still be found. Thirty seconds, an OVMX design value.
 */
#define MSCP_CL_RETRY_MS 30000u

/* ==========================================================================
 * 1. The object
 * ========================================================================== */

/* One member this driver has, or wants, an `MSCP$DISK` connection to. Every
 * field is a fact this layer established: the sysid SCS reported, the Con.ID
 * the allocator minted for OUR connect, and when we last tried. */
struct mscp_cl_conn {
	uint8_t         in_use;
	uint8_t         opened;
	uint8_t         pad[2];
	vms_scs_sysid_t peer;
	vms_conid_t     conid;
	uint32_t        last_try_ms;
};

struct vms_mscp_cl {
	struct vms_cluster   *cl;
	struct mscp_cl_fsm    fsm;
	struct mscp_cl_ops    ops;
	struct cnxman_disk_client_ops hook;

	struct mscp_cl_conn   conn[MSCP_CL_MAX_CTLRS];

	/* The ONE completion registration the asynchronous service (SS3) calls
	 * back through. A second call replaces it; NULL withdraws it. */
	vms_mscp_cl_done_cb   done_cb;
	void                 *done_ctx;

	/* Real events counted HERE because here is where they happen. */
	uint32_t connects;
	uint32_t connect_refusals;
	uint32_t closes;
	uint32_t devices_added;
	uint32_t devices_removed;
	uint32_t devtab_failures;
	uint32_t completions_dropped;   /* a completion with no registered cb */
};

/* ==========================================================================
 * 2. struct mscp_cl_ops -- DOWNWARD, to the executive
 *
 * Every entry is a one-line dereference into a real executive service. Nothing
 * here interprets MSCP, and nothing caches: each reads cl->scs / cl->pe fresh,
 * so a layer that has gone away yields an honest refusal instead of a
 * dereference of freed state.
 * ========================================================================== */

static int cl_op_send_cmd(void *ctx, vms_conid_t conid, const uint8_t *body,
			  uint32_t len)
{
	struct vms_mscp_cl *c = (struct vms_mscp_cl *)ctx;

	if (c->cl->scs == NULL)
		return -1;
	return scs_send_msg(c->cl->scs, conid, body, len) == (int)SS__NORMAL
		       ? 0 : -1;
}

/*
 * The ONE place this driver's direction vocabulary meets the port's. The pure
 * FSM names MSCP_CL_BUF_IN/_OUT precisely so it does not depend on the layer
 * it is injected over (design SS3.9 rule 1); the mapping lives here.
 */
static uint8_t cl_access_bits(uint8_t access)
{
	uint8_t bits = 0u;

	if ((access & MSCP_CL_BUF_IN) != 0u)
		bits |= (uint8_t)PE_BLK_ACC_DST;
	if ((access & MSCP_CL_BUF_OUT) != 0u)
		bits |= (uint8_t)PE_BLK_ACC_SRC;
	return bits;
}

static int cl_op_buf_register(void *ctx, uint8_t *base, uint32_t len,
			      uint8_t access, uint32_t *name_out)
{
	struct vms_mscp_cl *c = (struct vms_mscp_cl *)ctx;

	if (c->cl->pe == NULL)
		return -1;
	return pe_buf_register(c->cl->pe, base, len, cl_access_bits(access),
			       name_out) == (int)SS__NORMAL ? 0 : -1;
}

static void cl_op_buf_release(void *ctx, uint32_t name)
{
	struct vms_mscp_cl *c = (struct vms_mscp_cl *)ctx;

	if (c->cl->pe != NULL)
		(void)pe_buf_release(c->cl->pe, name);
}

/*
 * A served unit became real. The row is entered under the name the PURE layer
 * composed from real executive state -- this file does not spell it (see
 * vms_mscp_cl_io_fsm.h's "THE SERVED DEVICE'S NAME").
 */
static void cl_op_unit_ready(void *ctx, const struct mscp_cl_ucb *u)
{
	struct vms_mscp_cl *c = (struct vms_mscp_cl *)ctx;

	if (vms_devtab_add_served_disk(u->devnam) != 0) {
		c->devtab_failures++;
		exec_console_printf("vms: could not enter served disk %s\n",
				    u->devnam);
		return;
	}
	c->devices_added++;
	exec_console_printf("vms: served disk %s (unit %u, served by a cluster "
			    "member)\n", u->devnam, (unsigned)u->unit.unit);
}

static void cl_op_unit_gone(void *ctx, const struct mscp_cl_ucb *u)
{
	struct vms_mscp_cl *c = (struct vms_mscp_cl *)ctx;

	if (vms_devtab_remove_served_disk(u->devnam) == 0)
		c->devices_removed++;
}

static void cl_op_io_done(void *ctx, uint32_t handle, uint16_t status,
			  uint32_t bytes)
{
	struct vms_mscp_cl *c = (struct vms_mscp_cl *)ctx;

	if (c->done_cb == (vms_mscp_cl_done_cb)0) {
		c->completions_dropped++;
		return;
	}
	c->done_cb(c->done_ctx, handle, status, bytes);
}

static uint64_t cl_op_time_now(void *ctx)
{
	(void)ctx;
	return exec_time_now_vms();
}

static uint32_t cl_op_now_ms(void *ctx)
{
	(void)ctx;
	return (uint32_t)exec_ticks_ms();
}

static void cl_op_log(void *ctx, const char *msg)
{
	(void)ctx;
	if (msg != NULL)
		exec_console_printf("%s", msg);
}

static void cl_ops_bind(struct vms_mscp_cl *c)
{
	c->ops.send_cmd = cl_op_send_cmd;
	c->ops.buf_register = cl_op_buf_register;
	c->ops.buf_release = cl_op_buf_release;
	c->ops.unit_ready = cl_op_unit_ready;
	c->ops.unit_gone = cl_op_unit_gone;
	c->ops.io_done = cl_op_io_done;
	c->ops.time_now = cl_op_time_now;
	c->ops.now_ms = cl_op_now_ms;
	c->ops.log = cl_op_log;
	c->ops.ctx = c;
}

/* ==========================================================================
 * 3. The connection table
 * ========================================================================== */

static struct mscp_cl_conn *cl_conn_by_peer(struct vms_mscp_cl *c,
					    vms_scs_sysid_t peer)
{
	uint32_t i;

	for (i = 0; i < MSCP_CL_MAX_CTLRS; i++) {
		if (c->conn[i].in_use && c->conn[i].peer == peer)
			return &c->conn[i];
	}
	return NULL;
}

static struct mscp_cl_conn *cl_conn_by_conid(struct vms_mscp_cl *c,
					     vms_conid_t conid)
{
	uint32_t i;

	for (i = 0; i < MSCP_CL_MAX_CTLRS; i++) {
		if (c->conn[i].in_use && c->conn[i].conid == conid)
			return &c->conn[i];
	}
	return NULL;
}

static struct mscp_cl_conn *cl_conn_alloc(struct vms_mscp_cl *c)
{
	uint32_t i;

	for (i = 0; i < MSCP_CL_MAX_CTLRS; i++) {
		if (!c->conn[i].in_use) {
			memset(&c->conn[i], 0, sizeof(c->conn[i]));
			c->conn[i].in_use = 1u;
			return &c->conn[i];
		}
	}
	return NULL;
}

/*
 * The serving node's own advertised SCSNODE, READ off its CSB. Returns the
 * length, or 0 when the connection manager holds none for that system -- and
 * then the pure layer creates no device for its units, because a served disk
 * under a made-up name is a fabricated device (INV-6).
 *
 * THE CSB IS READ THROUGH ITS PURE ACCESSOR, NOT THROUGH cnxman_get_csb().
 * That projection takes the fork mutex (vms_cnxman.h SS6, "read-only
 * projections taken under the fork mutex") and every caller of this function is
 * ALREADY ON the fork context, which holds it -- exec_mutex_t is not
 * recursive, so the projection would deadlock the cluster thread on its own
 * lock. cnxman_club_find_sysid() is the fork-context spelling, and it is the
 * same one vms_cnxman.c's own on-context code uses.
 */
static uint32_t cl_peer_scsnode(struct vms_mscp_cl *c, vms_scs_sysid_t peer,
				uint8_t *out, uint32_t cap)
{
	struct vms_csb *csb = cnxman_club_find_sysid(&c->cl->club, peer);

	if (csb == NULL || !csb->sysid_valid || csb->scsnode_len == 0u)
		return 0u;
	if ((uint32_t)csb->scsnode_len > cap)
		return 0u;
	memcpy(out, csb->scsnode, (size_t)csb->scsnode_len);
	return (uint32_t)csb->scsnode_len;
}

/* ==========================================================================
 * 4. struct cnxman_disk_client_ops -- UPWARD, from SCS through CNXMAN
 *
 * One `VMS$DISK_CL_DRVR` registration, two consumers (vms_cnxman.h SS5b). Each
 * callback here first asks "is this Con.ID one *I* opened?" -- a fact this
 * layer holds -- and does nothing otherwise.
 * ========================================================================== */

static void cl_hook_opened(void *ctx, vms_conid_t local_conid)
{
	struct vms_mscp_cl *c = (struct vms_mscp_cl *)ctx;
	struct mscp_cl_conn *cn = cl_conn_by_conid(c, local_conid);
	uint8_t scsnode[VMS_SCSNODE_MAX + 2];
	uint32_t len;

	if (cn == NULL || cn->opened)
		return;      /* not ours, or already open */
	cn->opened = 1u;

	memset(scsnode, 0, sizeof(scsnode));
	len = cl_peer_scsnode(c, cn->peer, scsnode, (uint32_t)sizeof(scsnode));
	(void)mscp_cl_fsm_conn_open(&c->fsm, local_conid, cn->peer, scsnode,
				    len);
}

static int cl_hook_message(void *ctx, vms_conid_t local_conid,
			   const uint8_t *body, uint32_t len)
{
	struct vms_mscp_cl *c = (struct vms_mscp_cl *)ctx;

	if (cl_conn_by_conid(c, local_conid) == NULL)
		return -1;   /* somebody else's connection */
	return mscp_cl_fsm_end_msg(&c->fsm, local_conid, body, len);
}

static void cl_hook_closed(void *ctx, vms_conid_t local_conid, uint32_t reason)
{
	struct vms_mscp_cl *c = (struct vms_mscp_cl *)ctx;
	struct mscp_cl_conn *cn = cl_conn_by_conid(c, local_conid);

	(void)reason;
	if (cn == NULL)
		return;
	if (!cn->opened)
		c->connect_refusals++;   /* the member serves no disks */
	else
		c->closes++;
	/*
	 * The FSM withdraws the devices and aborts the requests; this layer
	 * only frees the slot and starts the retry clock, so the member is
	 * asked again later rather than hammered.
	 */
	mscp_cl_fsm_conn_closed(&c->fsm, local_conid);
	cn->opened = 0u;
	cn->conid = 0u;
	cn->last_try_ms = (uint32_t)exec_ticks_ms();
}

static void cl_hook_bind(struct vms_mscp_cl *c)
{
	c->hook.opened = cl_hook_opened;
	c->hook.message = cl_hook_message;
	c->hook.closed = cl_hook_closed;
	c->hook.ctx = c;
}

/* The block-transfer consumer (vms_scs.h SS9): one dereference into the
 * driver, which finds its own request by the buffer name THIS node minted. */
static void cl_block_data(void *ctx, uint32_t name, uint32_t offset,
			  uint32_t len, uint32_t bytes_remaining)
{
	struct vms_mscp_cl *c = (struct vms_mscp_cl *)ctx;

	mscp_cl_fsm_block_data(&c->fsm, name, offset, len, bytes_remaining);
}

/* ==========================================================================
 * 5. The beat
 * ========================================================================== */

/* Is this member due for a connect attempt? */
static int cl_conn_due(const struct mscp_cl_conn *cn, uint32_t now)
{
	if (cn->conid != 0u)
		return 0;   /* one is already open or opening */
	if (cn->last_try_ms == 0u)
		return 1;   /* never tried */
	return (uint32_t)(now - cn->last_try_ms) >= MSCP_CL_RETRY_MS;
}

/* Open one `MSCP$DISK` connection to `peer`, through CNXMAN's registration. */
static void cl_try_connect(struct vms_mscp_cl *c, struct mscp_cl_conn *cn,
			   uint32_t now)
{
	vms_conid_t conid = 0u;

	cn->last_try_ms = now;
	if (cnxman_disk_client_connect(c->cl, cn->peer, &conid) !=
		    (int)SS__NORMAL || conid == 0u) {
		c->connect_refusals++;
		return;
	}
	cn->conid = conid;
	c->connects++;
}

/*
 * ONE BEAT: sweep the systems SCS really has a circuit to, open an `MSCP$DISK`
 * connection to each one this driver does not already hold, and run the
 * driver's own deadline reaper. Nothing here asserts anything about a member
 * beyond the one fact SCS established -- a circuit exists to this SCSSYSTEMID.
 */
static void cl_sweep_peers(struct vms_mscp_cl *c, uint32_t now)
{
	uint32_t i;

	for (i = 0; i < MSCP_CL_MAX_CTLRS; i++) {
		vms_scs_sysid_t peer = 0u;
		struct mscp_cl_conn *cn;

		if (vms_scs_peer_at(c->cl, i, &peer) != (int)SS__NORMAL)
			break;   /* the honest end of SCS's own list */
		if (peer == 0u)
			continue;

		cn = cl_conn_by_peer(c, peer);
		if (cn == NULL) {
			cn = cl_conn_alloc(c);
			if (cn == NULL)
				break;   /* table full: counted by no slot */
			cn->peer = peer;
		}
		if (cl_conn_due(cn, now))
			cl_try_connect(c, cn, now);
	}
}

static void cl_beat(struct vms_mscp_cl *c)
{
	uint32_t now = (uint32_t)exec_ticks_ms();

	cl_sweep_peers(c, now);
	(void)mscp_cl_fsm_tick(&c->fsm);
}

static void cl_arm_beat(struct vms_mscp_cl *c)
{
	(void)cf_timer_arm(c->cl->fork, CF_OWNER_MSCP_CL, MSCP_CL_TIMER_BEAT,
			   0u, MSCP_CL_BEAT_MS);
}

static void cl_work_handler(void *ctx, const struct cf_work *w)
{
	struct vms_mscp_cl *c = (struct vms_mscp_cl *)ctx;

	if (c == NULL || w == NULL || w->kind != CF_WORK_TIMER)
		return;
	if (w->arg0 != MSCP_CL_TIMER_BEAT)
		return;   /* an identity this layer never armed: ignored */
	cl_beat(c);
	cl_arm_beat(c);
}

/* ==========================================================================
 * 6. Lifecycle
 * ========================================================================== */

int vms_mscp_cl_start(struct vms_cluster *cl)
{
	struct vms_mscp_cl *c;

	if (cl == NULL)
		return (int)SS__BADPARAM;
	if (cl->mscp_cl != NULL)
		return (int)SS__NORMAL;          /* already up: idempotent */
	if (cl->fork == NULL || cl->pe == NULL || cl->scs == NULL ||
	    cl->cnxman == NULL)
		return (int)SS__NOSUCHDEV;       /* Rule 9: no layer beneath */

	c = (struct vms_mscp_cl *)exec_zalloc(sizeof(*c));
	if (c == NULL)
		return (int)SS__INSFMEM;
	c->cl = cl;
	cl_ops_bind(c);
	cl_hook_bind(c);
	mscp_cl_fsm_init(&c->fsm, &c->ops);

	(void)cf_set_work_handler(cl->fork, CF_OWNER_MSCP_CL, cl_work_handler,
				  c);
	cl->mscp_cl = c;
	cnxman_set_disk_client(cl, &c->hook);
	(void)vms_scs_set_block_consumer(cl, cl_block_data, c);

	/*
	 * The first beat runs NOW rather than in a second: a node whose peers
	 * are already up starts looking for served disks immediately, and a
	 * node with no peers finds none -- which is the honest state either
	 * way.
	 */
	cl_beat(c);
	cl_arm_beat(c);
	return (int)SS__NORMAL;
}

void vms_mscp_cl_stop(struct vms_cluster *cl)
{
	struct vms_mscp_cl *c;
	uint32_t i;

	if (cl == NULL || cl->mscp_cl == NULL)
		return;
	c = cl->mscp_cl;

	/* Stop being told about transfers and connections before anything is
	 * torn down, so no completion can arrive mid-teardown. The block
	 * withdrawal NAMES this driver, so the MSCP server's own registration
	 * survives it (vms_scs.h SS9). */
	(void)vms_scs_set_block_consumer(cl, NULL, c);
	cnxman_set_disk_client(cl, NULL);

	/* Every served device this driver entered goes away with it: a device
	 * whose driver is gone is not reachable, and leaving the row would
	 * advertise a disk nothing can read. */
	for (i = 0; i < MSCP_CL_MAX_CTLRS; i++) {
		if (c->conn[i].in_use && c->conn[i].conid != 0u)
			mscp_cl_fsm_conn_closed(&c->fsm, c->conn[i].conid);
	}

	if (cl->fork != NULL) {
		cf_timer_cancel(cl->fork, CF_OWNER_MSCP_CL, MSCP_CL_TIMER_BEAT,
				0u);
		(void)cf_set_work_handler(cl->fork, CF_OWNER_MSCP_CL, NULL,
					  NULL);
	}

	cl->mscp_cl = NULL;
	exec_free(c);
}

/* ==========================================================================
 * 7. Readback
 * ========================================================================== */

void vms_mscp_cl_status(struct vms_cluster *cl, uint32_t *out_units,
			uint32_t *out_controllers)
{
	uint32_t units = 0u, ctlrs = 0u;

	if (cl != NULL && cl->mscp_cl != NULL) {
		uint32_t i;

		units = mscp_cl_fsm_unit_count(&cl->mscp_cl->fsm);
		for (i = 0; i < MSCP_CL_MAX_CTLRS; i++) {
			if (mscp_cl_fsm_cddb_at(&cl->mscp_cl->fsm, i) != NULL)
				ctlrs++;
		}
	}
	if (out_units != NULL)
		*out_units = units;
	if (out_controllers != NULL)
		*out_controllers = ctlrs;
}

/* ==========================================================================
 * 8. The asynchronous block service (vms_mscp_cl.h SS3)
 * ========================================================================== */

static int cl_io_entry(struct vms_cluster *cl, const char *devnam, uint32_t lbn,
		       uint32_t nblocks, uint8_t *buf, uint32_t buf_len,
		       uint32_t handle, vms_mscp_cl_done_cb done, void *done_ctx,
		       int is_write)
{
	struct vms_mscp_cl *c;
	int rc;

	if (cl == NULL || devnam == NULL || buf == NULL)
		return (int)SS__BADPARAM;
	if (cl->mscp_cl == NULL)
		return (int)SS__NOSUCHDEV;   /* no class driver: no served disk */
	c = cl->mscp_cl;
	c->done_cb = done;
	c->done_ctx = done_ctx;

	rc = is_write ? mscp_cl_fsm_write(&c->fsm, devnam, lbn, nblocks, buf,
					  buf_len, handle)
		      : mscp_cl_fsm_read(&c->fsm, devnam, lbn, nblocks, buf,
					 buf_len, handle);
	/* SS$_DEVOFFLINE is what this executive already uses for "the path or
	 * the connection cannot carry this right now" (vms_scs.c's own status
	 * map documents the choice and why an invented SS$_ would be worse). */
	return rc == 0 ? (int)SS__NORMAL : (int)SS__DEVOFFLINE;
}

int vms_mscp_cl_read(struct vms_cluster *cl, const char *devnam, uint32_t lbn,
		     uint32_t nblocks, uint8_t *buf, uint32_t buf_len,
		     uint32_t handle, vms_mscp_cl_done_cb done, void *done_ctx)
{
	return cl_io_entry(cl, devnam, lbn, nblocks, buf, buf_len, handle, done,
			   done_ctx, 0);
}

int vms_mscp_cl_write(struct vms_cluster *cl, const char *devnam, uint32_t lbn,
		      uint32_t nblocks, uint8_t *buf, uint32_t buf_len,
		      uint32_t handle, vms_mscp_cl_done_cb done, void *done_ctx)
{
	return cl_io_entry(cl, devnam, lbn, nblocks, buf, buf_len, handle, done,
			   done_ctx, 1);
}
