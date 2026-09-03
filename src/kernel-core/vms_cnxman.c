/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman.c - the Connection Manager's executive glue (FC-P3.8).
 *
 * FC-P3.3/P3.5/P3.6/P3.7/P3.12 built four PURE state machines (the join, the
 * transition barrier, the CLUB/CSB model + the ten-state connectivity ladder,
 * the quorum arithmetic, and the transition coordinator) plus the shared
 * Phase 2 commit (vms_cnxman_phase2.h). Every one of them reaches the world
 * only through an injected `struct cnxman_ops` and owns no storage of its own.
 *
 * THIS FILE IS WHAT TURNS THOSE INTO A RUNNING LAYER, on exactly the terms
 * vms_scs.c (FC-P2.4) and vms_pe.c (FC-P0.9) already set for their own layers:
 *
 *   - it OWNS the objects: `struct vms_cnxman` holds one join, one barrier,
 *     one coordinator and one reconnect-loop instance -- ONE CNXMAN per node,
 *     the CSBs already being the per-REMOTE-system state (design SS3.4). The
 *     pure layers still allocate nothing.
 *   - it BINDS `struct cnxman_ops` DOWNWARD to SCS: `send`/`respond` resolve a
 *     destination through the CSB's real `cdt_conid` (or, for a response, the
 *     conid the request just arrived on) and hand the body to `scs_send_msg`
 *     -- design sec 3.2.4 ruling E1's seam, "glue resolves the CSB's
 *     VMS$VAXcluster CDT -> scs_send_msg". Timers ride FC-P0.5's cf_timer_*
 *     wrappers under CF_OWNER_CNXMAN; `log` is the %CNXMAN/%VAXcluster OPA0:
 *     line (the FSMs already compose the whole message -- this file only
 *     prints it, exactly as scs_ops_log does).
 *   - it BINDS `struct cnxman_join_ops` the same way: SCS$DIRECTORY lookups,
 *     SYSAP connects/sends/disconnects, all through vms_scs.h.
 *   - it REGISTERS the `VMS$VAXcluster` and the disk-client-driver SYSAPs
 *     with SCS, and drives the CSB ten-state ladder (vms_cnxman_csb.h) off
 *     that SYSAP's own connect/open/close lifecycle -- the "SCS connection
 *     between the local SYS$CLUSTER and the remote one" the book's CSB model
 *     describes (pp. 7-23/7-24) IS the VMS$VAXcluster CDT.
 *   - it DISPATCHES an inbound `VMS$VAXcluster` message to the join, then the
 *     barrier, then the coordinator, in that order -- the shared
 *     `enum cnxman_event` vocabulary and each FSM's own NOT_MINE/HANDOFF
 *     contract is what makes that a plain three-step try, never a decision
 *     this file makes about the wire.
 *   - it fills `CLUSTER_DIAG_CSB` (vms_devtab.c calls cnxman_get_club/
 *     cnxman_get_csb/cnxman_find_csb below) and delivers `$SETCLUEVT`
 *     (SS7) and the operator-visible "node added/removed" lines (SS8).
 *
 * WHAT THIS FILE DOES NOT DO. It builds no frame and decodes none (the codec
 * owns that, reached only through the FSMs); it makes no protocol decision
 * (the FSMs own that); it never writes body[0:8] (cnxman_envelope_stamp is
 * the one function permitted to, called by the FSMs themselves before
 * `ops->send`/`respond` is reached); and it reaches the substrate only
 * through exec_kbackend.h and the FC-P0.5 fork API -- no <linux/...>, no
 * <sys/...>, ever (tools/ci/cluster_core_includes_gate.sh).
 *
 * SS3.5 E31 -- THE CONNECT DATA. `cnxman_join_cfg.conndata_valid` is left 0
 * here: this file bakes in no captured constant and supplies no substitute of
 * its own (the strawman's replayed `01 1b 01 03 ...` is exactly what INV-6
 * forbids). The join then sends an explicit counted zero and counts the
 * omission itself (vms_cnxman_join_fsm.h "WHAT THIS FILE REFUSES TO INVENT",
 * C). This is an OPERATOR-RESERVED identity decision (integration note E31),
 * not a default this glue may choose.
 *
 * SS3.7 E29 -- ACTING ON A CLOSE REASON. `scs_sysap_ops.closed()`'s `reason`
 * arrives here as the RAW `enum scs_close_reason` (design SS3.2.2 keeps SS$_
 * out of kernel-core cluster headers); this is the first SYSAP that acts on
 * one (routing SCS_CLOSE_REJECTED to `cnxman_join_rejected()` and every other
 * value to the CSB ladder / `cnxman_join_closed()`), and the %CNXMAN line this
 * file renders for a close is the first place that reason becomes text.
 *
 * INCLUDES: this TU is on the cluster core list enforced by
 * tools/ci/cluster_core_includes_gate.sh -- exec_kbackend.h and kernel-core
 * headers only, never a substrate header.
 */

#include "vms_internal.h"      /* the SS$_ vocabulary, struct vms_proc/ast_state */
#include "exec_kbackend.h"     /* the ONLY substrate surface this TU has */
#include "exec_list.h"         /* exec_list_add_tail -- the $SETCLUEVT AST queue */
#include "vms_cluster.h"
#include "vms_cluster_fork.h"  /* FC-P0.5: cf_timer_*, fork_enter/leave */
#include "vms_cluster_snapshot.h"
#include "vms_scs.h"            /* the SYSAP surface this glue registers on */
#include "vms_scs_fsm.h"        /* enum scs_close_reason (E29) */
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_join_fsm.h"
#include "vms_cnxman_barrier_fsm.h"
#include "vms_cnxman_coord_fsm.h"
#include "vms_cnxman_recnx_fsm.h"
#include "vms_cnxman_phase2.h"
#include "vms_cnxman_quorum.h"

/* ==========================================================================
 * 0. Sizes and the two SYSAP names this layer registers
 *
 * `VMS$VAXcluster` (symmetric: same 16 bytes on both ends) carries the whole
 * membership dialogue and every transition frame. The join's disk-client name
 * (vms_cnxman_join_fsm.h's `cnxman_join_name_disk_cl_drvr`) is registered too,
 * ONLY so scs_connect() -- which requires the LOCAL name to already be a
 * registered SYSAP (vms_scs.h SS5) -- can open the outbound MSCP$DISK client
 * connection the join drives; nobody may connect TO it (connect_req refuses).
 * ========================================================================== */
#define CNXMAN_VC_CREDITS   4u   /* one command/response at a time per peer */
#define CNXMAN_MSCP_CREDITS 4u   /* matches CNXMAN_JOIN_MSCP_CREDITS */

/*
 * The glue's own timer identity space, on CF_OWNER_CNXMAN. `enum cnxman_timer`
 * (vms_cnxman.h) already numbers RECNX=0, JOIN=1, BARRIER=2, COORD=3 -- used
 * directly as the cf_work `arg0`, exactly as vms_scs.c uses `enum scs_timer`.
 */

/*
 * The CNXMAN layer's objects. Opaque to every other TU (vms_cnxman.h says so
 * with a forward declaration only); this is the ONLY file that may look
 * inside it.
 */
struct vms_cnxman {
	struct vms_cluster *cl;

	struct cnxman_ops           ops;
	struct cnxman_join_ops      jops;
	struct scs_sysap_ops        vc_sysap;
	struct scs_sysap_ops        mscp_sysap;

	/*
	 * The DISK CLASS DRIVER's hook on the ONE `VMS$DISK_CL_DRVR`
	 * registration (vms_cnxman.h SS5b, FC-P7.1). NULL is a real
	 * configuration -- a node that mounts no served disk -- and then the
	 * join's own walk is the only consumer, exactly as before.
	 */
	struct cnxman_disk_client_ops disk_client;
	uint8_t                       disk_client_set;
	uint8_t                       pad_dc[3];

	struct cnxman_join    join;
	struct cnxman_barrier  barrier;
	struct cnxman_coord    coord;
	struct cnxman_recnx    recnx;

	/*
	 * The request currently being dispatched, for `ops->respond()`
	 * (design: "answer the request currently being dispatched, on its own
	 * connection"). Set immediately before a frame is handed to the join/
	 * barrier/coordinator and read by respond(); there is exactly one
	 * dispatch in flight at a time (the fork context serializes every
	 * event, FC-P0.5), so one scalar is the whole of this state.
	 */
	vms_conid_t     cur_conid;
	uint8_t         cur_conid_valid;
	struct vms_csb *cur_csb;   /* the CSB `cur_conid` resolved to, or NULL */

	/*
	 * An inbound VMS$VAXcluster connect was just ACCEPTED (connect_req
	 * returned 0) but has not yet reached OPEN: the peer identity the CSB
	 * ladder needs is known now, the connection's own Con.ID only at
	 * opened() (vms_scs.h: "local_conid is the LISTENING CDT's Con.ID, not
	 * the connection's ... the SYSAP learns it from opened()"). The fork
	 * context dispatches one SCS event at a time, so exactly one accept is
	 * ever in flight; a second one before the first's opened() arrives
	 * would overwrite this and is counted, never guessed at.
	 */
	vms_scs_sysid_t pending_accept_sysid;
	uint8_t         pending_accept_valid;
	uint32_t        pending_accept_overwritten;

	/* ---- $SETCLUEVT (SS7): a single registration, this node's own ----
	 * `proc` is an opaque handle (this header stays substrate-agnostic);
	 * vms_devtab.c's ioctl handler supplies it and vms_cnxman_proc_gone()
	 * clears it at process death so delivery never reaches freed memory.
	 * Guarded by the fork mutex, like every other field here. */
	void     *cluevt_proc;
	uint32_t  cluevt_mask;      /* CLUEVT$C_ADD | CLUEVT$C_REMOVE bits */
	uint64_t  cluevt_astadr;
	uint64_t  cluevt_astprm;

	/* ---- counted, never displayed as protocol state ---- */
	uint32_t frames_to_join;
	uint32_t frames_to_barrier;
	uint32_t frames_to_coord;
	uint32_t frames_unrouted;
	uint32_t cluevt_delivered;
	uint32_t cluevt_dropped;     /* quota exceeded or nobody registered */
	uint32_t proposed_fills;     /* E3: proposed_valid set from real state */
	uint32_t reconnects_issued;
	uint32_t peers_discovered;   /* E36: CSBs allocated from a real vc_up */
};

/* ==========================================================================
 * 1. CSB lookup helpers -- the only two ways this glue resolves a peer
 * ========================================================================== */

/* Find the CSB whose VMS$VAXcluster CDT is `conid` (0 never matches: it is
 * not a Con.ID any allocator mints). Linear scan over the live high-water
 * mark, bounded by VMS_CLUB_MAX_CSB -- the same cost cnxman_club_find_csid()
 * already pays for the identical reason (no second index to keep in step). */
static struct vms_csb *csb_by_conid(struct vms_club *club, vms_conid_t conid)
{
	uint32_t i;

	if (club == NULL || conid == 0u)
		return NULL;
	for (i = 0; i < club->n_csb; i++) {
		struct vms_csb *csb = &club->csb[i];

		if (csb->in_use && csb->cdt_conid == conid)
			return csb;
	}
	return NULL;
}

/* Find-or-allocate the CSB for `sysid` -- p. 7-23's "a newly discovered
 * connection manager", allocated in state NEW the first time this node
 * observes it, whichever direction observed it first. */
static struct vms_csb *csb_ensure(struct vms_club *club, vms_scs_sysid_t sysid)
{
	struct vms_csb *csb = cnxman_club_find_sysid(club, sysid);

	if (csb != NULL)
		return csb;
	return cnxman_club_alloc_csb(club, sysid, 1);
}

/* ==========================================================================
 * 2. struct cnxman_ops -- DOWNWARD, to SCS and the fork context
 * ========================================================================== */

/*
 * E1's transport half, PLUS the one bookkeeping job that belongs here and
 * nowhere else in the barrier/coordinator's own TUs: advancing the CSB's
 * outbound dialogue counters (cnxman_csb_dialogue_sent(), vms_cnxman_csb.h).
 * The join FSM resolves its own target CSB and calls this itself (design
 * sec 3.2.4 ruling E1's FC-P3.3 half); the barrier and the coordinator
 * resolve a CSB per destination but do NOT call it (confirmed by grep at
 * review time), because a message they hand to `ops->send`/`respond` has not
 * left the node until this glue's transport call actually succeeds -- so
 * this is the one place a real send is known to have happened.
 */
static int cnxman_ops_send(void *ctx, vms_csid_t dst, const uint8_t *body,
			   uint32_t len)
{
	struct vms_cnxman *cn = (struct vms_cnxman *)ctx;
	struct vms_csb *csb;
	int status;

	if (cn == NULL || cn->cl->scs == NULL)
		return SS__NOSUCHDEV;
	/*
	 * A destination this node cannot resolve to a real CSB is a refusal to
	 * transmit, never a zero-filled body (INV-6) -- the same rule the
	 * barrier's and coordinator's own headers state for envelope stamping.
	 * Today this is the ROUTINE case (integration note E30): no CSID is
	 * ever learned, so every CSID-addressed origination honestly fails
	 * here rather than fabricating a destination.
	 */
	csb = cnxman_club_find_csid(&cn->cl->club, dst);
	if (csb == NULL || !csb->in_use || csb->cdt_conid == 0u)
		return SS__NOSUCHDEV;
	status = scs_send_msg(cn->cl->scs, csb->cdt_conid, body, len);
	if (status == SS__NORMAL)
		cnxman_csb_dialogue_sent(csb);
	return status;
}

static int cnxman_ops_respond(void *ctx, const uint8_t *body, uint32_t len)
{
	struct vms_cnxman *cn = (struct vms_cnxman *)ctx;
	int status;

	if (cn == NULL || !cn->cur_conid_valid || cn->cl->scs == NULL)
		return SS__NOSUCHDEV;
	status = scs_send_msg(cn->cl->scs, cn->cur_conid, body, len);
	if (status == SS__NORMAL && cn->cur_csb != NULL)
		cnxman_csb_dialogue_sent(cn->cur_csb);
	return status;
}

static void cnxman_ops_arm_timer(void *ctx, enum cnxman_timer which,
				 uint32_t key, uint32_t ms)
{
	struct vms_cnxman *cn = (struct vms_cnxman *)ctx;

	(void)cf_timer_arm(cn->cl->fork, CF_OWNER_CNXMAN, (uint32_t)which, key,
			   ms);
}

static void cnxman_ops_cancel_timer(void *ctx, enum cnxman_timer which,
				    uint32_t key)
{
	struct vms_cnxman *cn = (struct vms_cnxman *)ctx;

	cf_timer_cancel(cn->cl->fork, CF_OWNER_CNXMAN, (uint32_t)which, key);
}

static uint32_t cnxman_ops_now_ms(void *ctx)
{
	(void)ctx;
	return (uint32_t)exec_ticks_ms();
}

/* The %CNXMAN / %VAXcluster OPA0: lines. Every join/barrier/coordinator/
 * recnx/CSB-ladder call site already composes the whole "%CNXMAN, ..." string
 * (grepped across all five .c files at review time); this is the one place
 * that reaches the console, exactly as scs_ops_log does for SCS. */
static void cnxman_ops_log(void *ctx, const char *msg)
{
	(void)ctx;
	if (msg != NULL)
		exec_console_printf("%s\n", msg);
}

static void cnxman_ops_bind(struct vms_cnxman *cn)
{
	cn->ops.send = cnxman_ops_send;
	cn->ops.respond = cnxman_ops_respond;
	cn->ops.arm_timer = cnxman_ops_arm_timer;
	cn->ops.cancel_timer = cnxman_ops_cancel_timer;
	cn->ops.now_ms = cnxman_ops_now_ms;
	cn->ops.log = cnxman_ops_log;
	cn->ops.alloc = NULL;   /* no FSM here allocates (design SS3.9 rule 3) */
	cn->ops.free = NULL;
	cn->ops.ctx = cn;
}

/* ==========================================================================
 * 3. struct cnxman_join_ops -- the join's SCS client surface
 * ========================================================================== */

/* scs_dir_lookup()'s callback thunk: hands the peer's real yes/no straight to
 * the join, which is the only code that interprets it. */
static void cnxman_jop_dir_cb(void *ctx, vms_scs_sysid_t from,
			      const uint8_t *name, int present)
{
	struct vms_cnxman *cn = (struct vms_cnxman *)ctx;

	cnxman_join_dir_result(&cn->join, from, name, present);
}

static int cnxman_jop_dir_inquire(void *ctx, vms_scs_sysid_t dst,
				  const uint8_t *name)
{
	struct vms_cnxman *cn = (struct vms_cnxman *)ctx;

	if (cn->cl->scs == NULL)
		return (int)SS__NOSUCHDEV;
	return scs_dir_lookup(cn->cl->scs, dst, name, cnxman_jop_dir_cb, cn);
}

/*
 * Open a connection FROM one of this node's own registered SYSAPs. Success
 * also feeds the CSB ladder -- but ONLY for the VMS$VAXcluster local name: the
 * ten-state model (pp. 7-23/7-24) describes the SCS connection between the
 * two systems' SYS$CLUSTER, which IS this connection, never the MSCP$DISK
 * client leg. `csb->cdt_conid` is set here immediately (not deferred to
 * opened()), so an outbound connect never needs the accept-pending slot
 * below: this node already knows both the peer and the Con.ID the moment
 * scs_connect() mints it.
 */
static int cnxman_jop_connect(void *ctx, vms_scs_sysid_t dst,
			      const uint8_t *local_name,
			      const uint8_t *remote_name,
			      const uint8_t *conndata, uint16_t credits,
			      vms_conid_t *out_conid)
{
	struct vms_cnxman *cn = (struct vms_cnxman *)ctx;
	int status;

	(void)conndata; /* scs_connect() carries no connect-data parameter of
			 * its own (vms_scs.h SS5); the 16-byte SCA field is
			 * SCS's own connect-verb payload (SS4(N)), built by
			 * the CONNECT-REQ frame itself, not passed through
			 * this glue-level seam. */
	(void)credits;  /* the registered SYSAP's OWN initial_credits governs
			 * (scs_connect() reads it from the registry, vms_scs.c
			 * SS8) -- there is no per-call override in vms_scs.h. */

	if (cn->cl->scs == NULL)
		return (int)SS__NOSUCHDEV;
	status = scs_connect(cn->cl->scs, local_name, remote_name, dst,
			     out_conid);
	if (status != (int)SS__NORMAL)
		return status;

	if (local_name == cnxman_join_name_vaxcluster) {
		struct vms_csb *csb = csb_ensure(&cn->cl->club, dst);

		if (csb != NULL) {
			csb->cdt_conid = *out_conid;
			(void)cnxman_csb_dispatch(&cn->cl->club, csb,
						  CNXMAN_CSB_EV_CONNECT_SENT,
						  &cn->ops);
		}
	}
	return (int)SS__NORMAL;
}

static int cnxman_jop_send_msg(void *ctx, vms_conid_t conid,
			       const uint8_t *body, uint32_t len)
{
	struct vms_cnxman *cn = (struct vms_cnxman *)ctx;

	if (cn->cl->scs == NULL)
		return (int)SS__NOSUCHDEV;
	return scs_send_msg(cn->cl->scs, conid, body, len);
}

static int cnxman_jop_disconnect(void *ctx, vms_conid_t conid)
{
	struct vms_cnxman *cn = (struct vms_cnxman *)ctx;

	if (cn->cl->scs == NULL)
		return (int)SS__NOSUCHDEV;
	return scs_disconnect(cn->cl->scs, conid, 0u);
}

/*
 * E24: the affirmative VMS$VAXcluster directory-answer descriptor. This glue
 * has no grounded 16 bytes to declare (spec SS4(h)(2) RE gap (c) is
 * undecoded, and vms_scs.h exposes no glue-level path to
 * scs_fsm_sysap_set_dir_data() -- that call is internal to vms_scs.c's own
 * registry). `cnxman_join_cfg.dir_descriptor_valid` is left 0 (below), so the
 * join FSM never calls this in practice; it is bound only so the ops table
 * has no NULL function pointer for a defensive caller to crash on.
 */
static int cnxman_jop_set_dir_data(void *ctx, const uint8_t *name,
				   const uint8_t *data)
{
	(void)ctx; (void)name; (void)data;
	return (int)SS__NOSUCHDEV;
}

/*
 * This node's VMS absolute time, for the MSCP SET CONTROLLER CHARACTERISTICS
 * P.TIME field. No absolute-time source is wired into the cluster fork
 * context; 0 is the field's own "no time supplied" (vms_cnxman_join_fsm.h),
 * an honest omission and not a fabricated clock reading.
 */
static uint64_t cnxman_jop_time_now(void *ctx)
{
	(void)ctx;
	return 0u;
}

static void cnxman_jops_bind(struct vms_cnxman *cn)
{
	cn->jops.dir_inquire = cnxman_jop_dir_inquire;
	cn->jops.connect = cnxman_jop_connect;
	cn->jops.send_msg = cnxman_jop_send_msg;
	cn->jops.disconnect = cnxman_jop_disconnect;
	cn->jops.set_dir_data = cnxman_jop_set_dir_data;
	cn->jops.time_now = cnxman_jop_time_now;
	cn->jops.ctx = cn;
}

/* ==========================================================================
 * 4. E3 -- the proposed->effective quorum copy actually has something to copy
 *
 * p. 7-42 task 2 copies PROPOSED quorum cells to EFFECTIVE ones, but no
 * capture isolates where a coordinator's Phase 1 open places its proposed
 * quorum/votes (vms_cnxman_coord_fsm.h SS "WHAT THIS FILE HONESTLY DOES NOT":
 * "their bytes go out zero"), so nothing upstream of this glue ever fills
 * club->proposed_* or sets proposed_valid (integration note E3). Leaving it
 * unset forever would make the copy a permanent no-op and the effective
 * quorum permanently stale the first time a transition actually reaches the
 * GO.
 *
 * This glue fills the proposed cells from the CLUB's own REAL running values
 * -- old CEVOTES/QUORUM/QDSKVOTES/member count, each one either a live
 * SYSGEN-derived tracked figure (cnxman_quorum_qdskvotes()) or the FC-P3.7
 * arithmetic's last real answer (club->cevotes/quorum) -- the moment a
 * transition opens and before its GO can arrive. Nothing here is invented:
 * every value copied already lived in the CLUB. Idempotent per transition
 * (proposed_valid gates it, and phase2_commit_quorum() clears the flag after
 * consuming it), called after every frame that reaches the barrier or the
 * coordinator.
 * ========================================================================== */
static void cnxman_glue_preload_proposed(struct vms_cnxman *cn)
{
	struct vms_club *club = &cn->cl->club;

	if (club->proposed_valid)
		return;
	/* Nothing to preload before a transition has ever opened -- an idle
	 * CLUB has no "proposed" anything and setting the flag here would be
	 * the fabricated-quorum-of-zero INV-6 forbids outright. */
	if (!club->transition_active)
		return;

	club->proposed_members = club->cluster_nodes;
	club->proposed_cevotes = club->cevotes;
	club->proposed_quorum = club->quorum;
	club->proposed_qdisk_votes = cnxman_quorum_qdskvotes(club);
	club->proposed_valid = 1u;
	cn->proposed_fills++;
}

/* ==========================================================================
 * 5. Membership-change detection -> the operator line + $SETCLUEVT
 *
 * phase2_commit_local_membership()/phase2_apply_nodemap() (vms_cnxman_
 * phase2.c, shared by the barrier and the coordinator) are the ONLY code that
 * ever sets or clears a CSB's MEMBER flag, and they run entirely inside the
 * barrier's/coordinator's own rx_frame call -- this glue cannot see the
 * moment it happens except by comparing before and after. A snapshot taken
 * around each dispatch is the only honest way to notice the edge without
 * duplicating phase2's own logic here (which would be the two-implementations
 * risk vms_cnxman_phase2.h's own header warns against).
 * ========================================================================== */

static void cnxman_membership_snapshot(struct vms_club *club, uint8_t *out)
{
	uint32_t i;

	for (i = 0; i < VMS_CLUB_MAX_CSB; i++)
		out[i] = 0u;
	for (i = 0; i < club->n_csb; i++) {
		struct vms_csb *csb = &club->csb[i];

		if (csb->in_use)
			out[i] = (uint8_t)cnxman_csb_is_member(csb);
	}
}

/* Queue a $SETCLUEVT completion AST, on exactly the terms vms_lock.c's own
 * queue_completion_ast() already uses for lock completions: a non-sleeping
 * allocation, the process's own PSL_C_USER queue, the quota check, and
 * vms_ast_notify_arrival() to wake a hibernating reader. `struct vms_proc`
 * and its `ast[]` array are the SAME struct every other kernel-core facility
 * (vms_lock.c, vms_ast.c) already reaches through "vms_internal.h" -- this is
 * not a new delivery mechanism, it is the established one. */
static void cnxman_deliver_cluevt(struct vms_cnxman *cn,
				  enum cnxman_cluster_event ev)
{
	struct vms_ast_entry *ast;
	struct vms_ast_state *ast_state;
	struct vms_proc *proc;
	uint32_t bit = (ev == CNXMAN_CLUEVT_ADD) ? 1u : 2u;

	if (cn->cluevt_proc == NULL || (cn->cluevt_mask & bit) == 0u) {
		cn->cluevt_dropped++;
		return;
	}
	proc = (struct vms_proc *)cn->cluevt_proc;

	ast = (struct vms_ast_entry *)exec_zalloc_atomic(sizeof(*ast));
	if (ast == NULL) {
		cn->cluevt_dropped++;
		return;
	}
	ast->astadr = cn->cluevt_astadr;
	ast->astprm = cn->cluevt_astprm;
	ast->acmode = PSL_C_USER;

	ast_state = &proc->ast[PSL_C_USER];
	exec_lock(&ast_state->lock);
	if (ast_state->count < VMS_AST_MAX_PER_MODE) {
		exec_list_add_tail(&ast->list, &ast_state->pending);
		ast_state->count++;
		exec_unlock(&ast_state->lock);
		vms_ast_notify_arrival(proc);
		cn->cluevt_delivered++;
	} else {
		exec_unlock(&ast_state->lock);
		exec_free(ast);
		cn->cluevt_dropped++;
	}
}

/* The operator-visible "node added/removed" line (plan row: "%CNXMAN/
 * %VAXcluster OPA0: lines ... the operator-visible membership messages").
 * Every field printed is real CSB state: the peer's own advertised
 * SCSSYSTEMID, never an invented node name (SCSNODE may not be learned). */
static void cnxman_log_membership_change(struct vms_cnxman *cn,
					 const struct vms_csb *csb, int added)
{
	char line[96];

	(void)snprintf(line, sizeof(line),
		      "%%CNXMAN, system %08x%08x %s the cluster",
		      (unsigned)((csb->sysid >> 32) & 0xffffffffu),
		      (unsigned)(csb->sysid & 0xffffffffu),
		      added ? "was added to" : "was removed from");
	cnxman_ops_log(cn, line);
}

static void cnxman_notify_membership_changes(struct vms_cnxman *cn,
					     const uint8_t *before)
{
	struct vms_club *club = &cn->cl->club;
	uint32_t i;

	for (i = 0; i < club->n_csb; i++) {
		struct vms_csb *csb = &club->csb[i];
		uint8_t after;

		if (!csb->in_use)
			continue;
		after = (uint8_t)cnxman_csb_is_member(csb);
		if (after == before[i])
			continue;
		cnxman_log_membership_change(cn, csb, after);
		cnxman_deliver_cluevt(cn, after ? CNXMAN_CLUEVT_ADD
						: CNXMAN_CLUEVT_REMOVE);
	}
}

/* ==========================================================================
 * 6. struct scs_sysap_ops -- the VMS$VAXcluster registration
 *
 * The CSB ten-state ladder (vms_cnxman_csb.h) is driven from exactly this
 * SYSAP's own connect/open/close lifecycle -- p. 7-23's "the state of the SCS
 * connection between the local SYS$CLUSTER and the remote one" IS this
 * connection.
 * ========================================================================== */

static int cnxman_vc_connect_req(void *ctx, vms_conid_t local_conid,
				 vms_scs_sysid_t peer, vms_conid_t peer_conid,
				 const uint8_t *conndata, uint32_t conndata_len)
{
	struct vms_cnxman *cn = (struct vms_cnxman *)ctx;
	struct vms_csb *csb;
	int rc;

	(void)local_conid; (void)peer_conid;

	/* THE SERVER HALF (spec SS4(y)): total connectivity requires this node
	 * accept every member's own connect. cnxman_join_connect_req() is that
	 * policy end to end, including recording the peer's connect-data
	 * (never acted on -- see its own header). */
	rc = cnxman_join_connect_req(&cn->join, peer, peer_conid, conndata,
				     conndata_len);
	if (rc != 0)
		return rc;

	csb = csb_ensure(&cn->cl->club, peer);
	if (csb != NULL)
		(void)cnxman_csb_dispatch(&cn->cl->club, csb,
					  CNXMAN_CSB_EV_CONNECT_RCVD, &cn->ops);

	if (cn->pending_accept_valid)
		cn->pending_accept_overwritten++;
	cn->pending_accept_sysid = peer;
	cn->pending_accept_valid = 1u;
	return 0;
}

static void cnxman_vc_opened(void *ctx, vms_conid_t local_conid)
{
	struct vms_cnxman *cn = (struct vms_cnxman *)ctx;
	struct vms_csb *csb;

	/* The INBOUND half: bind the Con.ID this SYSAP just learned to the
	 * peer identity connect_req recorded (see the pending-slot's own
	 * comment on struct vms_cnxman above). An OUTBOUND connect already set
	 * cdt_conid in cnxman_jop_connect(); csb_by_conid() finds that CSB too,
	 * so both halves reach the SAME CNXMAN_CSB_EV_CONN_OPEN dispatch below
	 * without this function needing to know which one it is. */
	if (cn->pending_accept_valid) {
		csb = csb_ensure(&cn->cl->club, cn->pending_accept_sysid);
		if (csb != NULL)
			csb->cdt_conid = local_conid;
		cn->pending_accept_valid = 0u;
	}

	csb = csb_by_conid(&cn->cl->club, local_conid);
	if (csb != NULL)
		(void)cnxman_csb_dispatch(&cn->cl->club, csb,
					  CNXMAN_CSB_EV_CONN_OPEN, &cn->ops);

	cnxman_join_opened(&cn->join, local_conid);
}

static int cnxman_vc_message(void *ctx, vms_conid_t local_conid,
			     const uint8_t *body, uint32_t len)
{
	struct vms_cnxman *cn = (struct vms_cnxman *)ctx;
	struct vms_club *club = &cn->cl->club;
	struct vms_csb *csb = csb_by_conid(club, local_conid);
	vms_csid_t from_csid = 0u;
	int32_t from_csb = -1;
	int from_valid = 0;
	uint8_t before[VMS_CLUB_MAX_CSB];
	enum cnxman_join_rx jrx;

	if (csb != NULL) {
		from_csb = (int32_t)cnxman_club_csb_index(club, csb);
		if (csb->csid_valid) {
			from_csid = csb->csid;
			from_valid = 1;
		}
	}

	cn->cur_conid = local_conid;
	cn->cur_conid_valid = 1u;
	cn->cur_csb = csb;
	cnxman_membership_snapshot(club, before);

	jrx = cnxman_join_rx_frame(&cn->join, body, len, from_csid, from_valid);
	if (jrx == CNXMAN_JOIN_RX_CONSUMED) {
		/* The join resolves and advances its OWN target CSB's dialogue
		 * counters internally (cnxman_csb_dialogue_sent/_heard,
		 * vms_cnxman_join_fsm.c) -- nothing to do here for it. */
		cn->frames_to_join++;
		cnxman_glue_preload_proposed(cn);
		cnxman_notify_membership_changes(cn, before);
		return 0;
	}

	/*
	 * Frames reaching here are NOT the join's own dialogue, and neither
	 * the barrier nor the coordinator records the peer's send-msg#
	 * themselves (grep-confirmed at review time) -- this is the ONE place
	 * that bookkeeping happens for them, exactly once per frame (design
	 * sec 3.2.4 ruling E1's FC-P3.8 half; spec sec 4(j): "ack-msg#
	 * acknowledges the peer's highest send-msg# seen").
	 */
	if (csb != NULL) {
		struct vms_frame_info fi;
		struct vms_cm_envelope env;

		if (vms_frame_classify(body, len, &fi) == VMS_CODEC_OK &&
		    vms_cm_envelope_parse(body, len, &fi, &env) == VMS_CODEC_OK)
			cnxman_csb_dialogue_heard(csb, env.send_msg);
	}

	{
		enum cnxman_barrier_rx brx =
			cnxman_barrier_rx_frame(&cn->barrier, body, len,
						from_csid, from_valid);

		if (brx == CNXMAN_BARRIER_RX_CONSUMED) {
			cn->frames_to_barrier++;
			cnxman_glue_preload_proposed(cn);
			cnxman_notify_membership_changes(cn, before);
			return 0;
		}
	}

	{
		enum cnxman_coord_rx crx =
			cnxman_coord_rx_frame(&cn->coord, body, len, from_csb);

		if (crx == CNXMAN_COORD_RX_CONSUMED) {
			cn->frames_to_coord++;
			cnxman_glue_preload_proposed(cn);
			cnxman_notify_membership_changes(cn, before);
			return 0;
		}
	}

	/* No FSM claimed it: an out-of-sequence or ungrounded frame. Counted,
	 * never silently dropped (scs_sysap_ops.message's own contract). */
	cn->frames_unrouted++;
	cnxman_ops_log(cn, "%CNXMAN, an unroutable VMS$VAXcluster frame was "
			   "received");
	return 1;
}

/*
 * E29: the first SYSAP to act on `enum scs_close_reason`. A REJECT is a
 * DIFFERENT fact from every other close (book p. 2-25's version gate, D12)
 * and is routed to cnxman_join_rejected(); everything else -- a matched
 * DISCONNECT, a lost path, a withdrawn SYSAP -- goes through the CSB ladder's
 * own connectivity-lost handling, which is what decides RECONNECT vs
 * PROPOSE_TRANSITION (p. 7-30), and then to cnxman_join_closed() so the join
 * (if this was one of its own two connections) sees the fact too.
 */
static void cnxman_vc_closed(void *ctx, vms_conid_t local_conid,
			     uint32_t reason)
{
	struct vms_cnxman *cn = (struct vms_cnxman *)ctx;
	struct vms_csb *csb = csb_by_conid(&cn->cl->club, local_conid);

	if (reason == (uint32_t)SCS_CLOSE_REJECTED) {
		cnxman_join_rejected(&cn->join, local_conid, reason);
		return;
	}

	if (csb != NULL) {
		/*
		 * `announced_departure` is p. 7-29's last-gasp distinction.
		 * `enum scs_close_reason` (vms_scs_fsm.h) has no last-gasp
		 * value distinct from a generic path loss, so this glue
		 * cannot honestly claim to have detected an announcement --
		 * passing 1 here without that signal would be exactly the
		 * kind of invented fact INV-6 forbids. Every close is
		 * therefore the p. 7-30 reconnect-window path until a port-
		 * level last-gasp signal exists to feed the true value.
		 */
		enum cnxman_csb_action act =
			cnxman_recnx_connectivity_lost(&cn->recnx, csb, 0);

		switch (act) {
		case CNXMAN_CSB_ACT_RECONNECT: {
			vms_conid_t new_conid = 0u;
			int rc;

			if (cn->cl->scs == NULL)
				break;
			rc = scs_connect(cn->cl->scs,
					 cnxman_join_name_vaxcluster,
					 cnxman_join_name_vaxcluster,
					 csb->sysid, &new_conid);
			if (rc == (int)SS__NORMAL) {
				csb->cdt_conid = new_conid;
				cn->reconnects_issued++;
				(void)cnxman_csb_dispatch(&cn->cl->club, csb,
							  CNXMAN_CSB_EV_CONNECT_SENT,
							  &cn->ops);
			}
			break;
		}
		case CNXMAN_CSB_ACT_PROPOSE_TRANSITION: {
			int32_t idx = (int32_t)cnxman_club_csb_index(
				&cn->cl->club, csb);

			(void)cnxman_coord_propose_remove(&cn->coord, idx);
			cnxman_glue_preload_proposed(cn);
			break;
		}
		default:
			break;
		}
	}

	cnxman_join_closed(&cn->join, local_conid, reason);
}

static void cnxman_vc_send_failed(void *ctx, vms_conid_t local_conid,
				  uint32_t reason)
{
	(void)ctx; (void)local_conid; (void)reason;
	/* SCS never retries (design SS3.2.5); there is nothing to do beyond
	 * what the CDT's own close (above) already handles when the circuit
	 * itself is what failed. */
}

static void cnxman_vc_sysap_bind(struct vms_cnxman *cn)
{
	cn->vc_sysap.connect_req = cnxman_vc_connect_req;
	cn->vc_sysap.opened = cnxman_vc_opened;
	cn->vc_sysap.message = cnxman_vc_message;
	cn->vc_sysap.closed = cnxman_vc_closed;
	cn->vc_sysap.send_failed = cnxman_vc_send_failed;
	cn->vc_sysap.ctx = cn;
}

/* ==========================================================================
 * 7. struct scs_sysap_ops -- the MSCP client-driver local name
 *
 * CLIENT ONLY: this node connects OUT as this name (the join's discovery
 * walk); nobody may connect IN to it, so connect_req refuses.
 * ========================================================================== */

static int cnxman_mscp_connect_req(void *ctx, vms_conid_t local_conid,
				   vms_scs_sysid_t peer,
				   vms_conid_t peer_conid,
				   const uint8_t *conndata, uint32_t len)
{
	(void)ctx; (void)local_conid; (void)peer; (void)peer_conid;
	(void)conndata; (void)len;
	return (int)SS__NOSUCHDEV;
}

/*
 * THE FAN-OUT (vms_cnxman.h SS5b). One SYSAP registration, two consumers: the
 * join's discovery walk and the disk class driver. Neither guesses which
 * connection is whose -- the join already ignores a Con.ID that is not the one
 * it opened, and the class driver ignores a Con.ID it holds no CDDB for -- so
 * handing each event to both is a demux on facts each of them holds, not a
 * routing decision made here.
 */
static const struct cnxman_disk_client_ops *cnxman_dc(struct vms_cnxman *cn)
{
	return cn->disk_client_set ? &cn->disk_client
				   : (const struct cnxman_disk_client_ops *)0;
}

static void cnxman_mscp_opened(void *ctx, vms_conid_t local_conid)
{
	struct vms_cnxman *cn = (struct vms_cnxman *)ctx;
	const struct cnxman_disk_client_ops *dc = cnxman_dc(cn);

	/* Advance the JOIN's MSCP_CONNECT step: the join opens MSCP$DISK before
	 * the VMS$VAXcluster VC (sequential states MSCP_CONNECT -> VC_CONNECT),
	 * so this CDT-open is the one join_h_mscp_opened is waiting for. Mirrors
	 * cnxman_vc_opened's cnxman_join_opened() call -- WITHOUT the membership
	 * CSB dispatch, because the MSCP CDT is a disk-client connection, not a
	 * VMS$VAXcluster membership block. (E43: without this, j->mscp_open is
	 * never set on a real wire; only the fake-ops R1 path reached the FSM.) */
	cnxman_join_opened(&cn->join, local_conid);

	if (dc != NULL && dc->opened != NULL)
		dc->opened(dc->ctx, local_conid);
}

static int cnxman_mscp_message(void *ctx, vms_conid_t local_conid,
			       const uint8_t *body, uint32_t len)
{
	struct vms_cnxman *cn = (struct vms_cnxman *)ctx;
	const struct cnxman_disk_client_ops *dc = cnxman_dc(cn);

	cnxman_join_rx_mscp(&cn->join, local_conid, body, len);
	if (dc != NULL && dc->message != NULL)
		(void)dc->message(dc->ctx, local_conid, body, len);
	return 0;
}

static void cnxman_mscp_closed(void *ctx, vms_conid_t local_conid,
			       uint32_t reason)
{
	struct vms_cnxman *cn = (struct vms_cnxman *)ctx;
	const struct cnxman_disk_client_ops *dc = cnxman_dc(cn);

	cnxman_join_closed(&cn->join, local_conid, reason);
	if (dc != NULL && dc->closed != NULL)
		dc->closed(dc->ctx, local_conid, reason);
}

static void cnxman_mscp_sysap_bind(struct vms_cnxman *cn)
{
	cn->mscp_sysap.connect_req = cnxman_mscp_connect_req;
	cn->mscp_sysap.opened = cnxman_mscp_opened;
	cn->mscp_sysap.message = cnxman_mscp_message;
	cn->mscp_sysap.closed = cnxman_mscp_closed;
	cn->mscp_sysap.send_failed = NULL;
	cn->mscp_sysap.ctx = cn;
}

/* ==========================================================================
 * 7b. PEER DISCOVERY -> CSB (FC-P3.9, integration note E36)
 *
 * THE GAP THIS CLOSES. The port learns a peer's SCSSYSTEMID off a real
 * received frame and raises vc_up; SCS records it on that system's SB and
 * stops there, because "SCS does not connect on its own; the SYSAP does"
 * (design SS3.2.5). Nothing turned that into "the CLUB has a block for this
 * system", so join_select_target() -- which walks the CLUB looking for a CSB
 * with a real sysid -- found nothing and every join returned NO_TARGET even
 * with a member sitting on the same LAN.
 *
 * WHAT IT DOES. Once per reconnect beat, sweep SCS's SB table
 * (vms_scs_peer_at(), vms_scs.h SS8) and allocate a CSB for each system with
 * an OPEN circuit that the CLUB has no block for yet -- p. 7-23's "newly
 * discovered connection manager", allocated in state NEW. That is ALL it
 * does: it opens no connection, sends nothing, and asserts nothing about the
 * system beyond the one fact the port established (a circuit exists, to this
 * SCSSYSTEMID). The CSB it allocates is NOT a member and does not claim to
 * be; only phase2 ever sets the MEMBER flag, and only from a real membership
 * record.
 *
 * INV-6: the sysid comes from vms_pe_fsm.c's vc_notify_up, read off the
 * wire -- never from a config file, a module parameter or a guess.
 * ========================================================================== */

/* One sweep pass. Returns the number of CSBs newly allocated (0 on a beat
 * where nothing new appeared, which is the normal case). */
static uint32_t cnxman_discover_peers(struct vms_cnxman *cn)
{
	uint32_t i, discovered = 0u;

	for (i = 0; i < VMS_CLUB_MAX_CSB; i++) {
		vms_scs_sysid_t sysid = 0;
		struct vms_csb *csb;

		if (vms_scs_peer_at(cn->cl, i, &sysid) != (int)SS__NORMAL)
			continue;   /* free slot or a circuit that is down */
		if (cnxman_club_find_sysid(&cn->cl->club, sysid) != NULL)
			continue;   /* already have this system's block */

		csb = cnxman_club_alloc_csb(&cn->cl->club, sysid, 1);
		if (csb == NULL)
			break;      /* CLUB full: counted by the CLUB, not here */
		discovered++;
		cnxman_ops_log(cn, "%CNXMAN, a connection manager was "
				   "discovered on the interconnect");
	}
	cn->peers_discovered += discovered;
	return discovered;
}

/*
 * Is there a system this node could join THROUGH right now? The same question
 * join_select_target() asks (a non-local CSB carrying a real SCSSYSTEMID) --
 * asked here so the glue can decide whether to drive a join at all, instead of
 * driving one that can only fail and leave the join FSM in FAILED where no
 * later discovery can restart it.
 */
static int cnxman_join_target_present(const struct vms_cnxman *cn)
{
	const struct vms_club *club = &cn->cl->club;
	uint32_t i;

	for (i = 0; i < club->n_csb; i++) {
		const struct vms_csb *c = &club->csb[i];

		if (c->in_use && c->sysid_valid &&
		    (c->flags & VMS_CSB_F_LOCAL) == 0u)
			return 1;
	}
	return 0;
}

/*
 * Drive the join if -- and only if -- there is somewhere to drive it to and
 * this node has not already started or finished one. Called at CLUSTER_START
 * and again on every beat that discovers a peer, which is what makes
 * VAXCLUSTER=2's "waiting to form or join" a real wait rather than a single
 * failed attempt at boot.
 *
 * Returns nonzero iff a join was started on this call.
 */
static int cnxman_join_drive(struct vms_cnxman *cn)
{
	if (cn->join.state != (uint8_t)CNXMAN_JOIN_IDLE)
		return 0;   /* already running, already joined, or FAILED */
	if (!cnxman_join_target_present(cn))
		return 0;

	(void)cnxman_join_start(&cn->join);
	if (cn->join.state == (uint8_t)CNXMAN_JOIN_FAILED)
		return 0;

	cn->cl->state = VMS_CLUSTER_JOINING;
	return 1;
}

/* ==========================================================================
 * 8. The fork thread's timer work handler (CONTRACT RULE 2: timers RUN here)
 * ========================================================================== */

/* One CNXMAN_CSB_ACT_* record from the reconnect beat, acted on exactly like
 * cnxman_vc_closed()'s own switch above -- the two are the same action
 * vocabulary because both come from the same CSB ladder. */
static void cnxman_act_on_recnx_rec(struct vms_cnxman *cn,
				    const struct cnxman_recnx_rec *rec)
{
	struct vms_csb *csb =
		cnxman_club_csb_at(&cn->cl->club, rec->csb_index);

	if (csb == NULL)
		return;

	switch ((enum cnxman_csb_action)rec->action) {
	case CNXMAN_CSB_ACT_RECONNECT: {
		vms_conid_t new_conid = 0u;
		int rc;

		if (cn->cl->scs == NULL)
			break;
		rc = scs_connect(cn->cl->scs, cnxman_join_name_vaxcluster,
				 cnxman_join_name_vaxcluster, csb->sysid,
				 &new_conid);
		if (rc == (int)SS__NORMAL) {
			csb->cdt_conid = new_conid;
			cn->reconnects_issued++;
			(void)cnxman_csb_dispatch(&cn->cl->club, csb,
						  CNXMAN_CSB_EV_CONNECT_SENT,
						  &cn->ops);
		}
		break;
	}
	case CNXMAN_CSB_ACT_PROPOSE_TRANSITION:
		(void)cnxman_coord_propose_remove(&cn->coord,
						  (int32_t)rec->csb_index);
		cnxman_glue_preload_proposed(cn);
		break;
	default:
		break;
	}
}

static void cnxman_work_handler(void *ctx, const struct cf_work *w)
{
	struct vms_cnxman *cn = (struct vms_cnxman *)ctx;
	struct cnxman_recnx_rec recs[VMS_CLUB_MAX_CSB];
	uint32_t n, i;

	if (cn == NULL || w == NULL || w->kind != CF_WORK_TIMER)
		return;

	switch ((enum cnxman_timer)w->arg0) {
	case CNXMAN_TIMER_RECNX:
		/* E36: discovery FIRST, so a system that appeared since the
		 * last beat has a CSB before the reconnect ladder and the join
		 * look at the CLUB on this same beat. */
		if (cnxman_discover_peers(cn) != 0u)
			(void)cnxman_join_drive(cn);
		n = cnxman_recnx_tick(&cn->recnx, recs, VMS_CLUB_MAX_CSB);
		for (i = 0; i < n; i++)
			cnxman_act_on_recnx_rec(cn, &recs[i]);
		break;
	case CNXMAN_TIMER_JOIN:
		cnxman_join_timer(&cn->join);
		break;
	case CNXMAN_TIMER_BARRIER:
		cnxman_barrier_timer(&cn->barrier);
		break;
	case CNXMAN_TIMER_COORD:
		cnxman_coord_timer(&cn->coord);
		break;
	default:
		/* An identity this layer never armed. */
		break;
	}
}

/* ==========================================================================
 * 9. Lifecycle
 * ========================================================================== */

/*
 * THE VAXCLUSTER DECISION, taken once at CLUSTER_START (FC-P3.9).
 *
 * VMS's convention, and what each value means HERE:
 *
 *   1 "a member only when a cluster is PRESENT". Presence is a fact about
 *     the interconnect, so it is asked of the interconnect: sweep for peers
 *     the port has already formed a circuit to and join through one if there
 *     is one. If there is not, this node is STANDALONE -- the true answer at
 *     the instant STARTUP.EXE asks, which is what it reports on the console.
 *     A member that appears LATER is still a cluster becoming present, so the
 *     reconnect beat's own sweep may still join it; the state then moves
 *     STANDALONE -> JOINING, and nothing that was already printed becomes a
 *     lie (it was true when it was printed).
 *
 *   2 "always a member". A node with nowhere to join is not standalone, it is
 *     WAITING -- VMS's own "waiting to form or join an OpenVMS Cluster" on
 *     OPA0:, which this emits once. The state stays JOINING and the sweep
 *     keeps looking. Non-blocking: SYSINIT's wait is a wait for an EVENT, not
 *     a sleep inside an ioctl, and the event is the peer sweep.
 *
 * NOTHING HERE FABRICATES A MEMBERSHIP. Only phase2 ever sets
 * VMS_CLUSTER_MEMBER, and only from a real membership record naming this
 * node's own SCSSYSTEMID (integration note E30). This function's strongest
 * output is JOINING.
 */
static void cnxman_start_join_or_wait(struct vms_cnxman *cn)
{
	struct vms_cluster *cl = cn->cl;

	(void)cnxman_discover_peers(cn);
	if (cnxman_join_drive(cn))
		return;

	if (cl->params.vaxcluster == 2u) {
		cl->state = VMS_CLUSTER_JOINING;
		cnxman_ops_log(cn, "%CNXMAN, waiting to form or join an "
				   "OpenVMS Cluster");
	} else {
		cl->state = VMS_CLUSTER_STANDALONE;
	}
}

int vms_cnxman_start(struct vms_cluster *cl)
{
	struct vms_cnxman *cn;
	struct cnxman_join_cfg cfg;
	int status;

	if (cl == NULL)
		return (int)SS__BADPARAM;
	if (cl->cnxman != NULL)
		return (int)SS__NORMAL;   /* already up: idempotent */

	/* VAXCLUSTER=0: returns immediately, state stays VMS_CLUSTER_OFF
	 * (vms_cnxman.h's own contract for this function). Nothing is
	 * instantiated -- the same "no fabricated layer" posture vms_pe_start
	 * and vms_scs_start already apply to VAXCLUSTER=0. */
	if (cl->params.vaxcluster == 0u)
		return (int)SS__NORMAL;

	if (cl->fork == NULL || cl->pe == NULL || cl->scs == NULL)
		return (int)SS__NOSUCHDEV;   /* Rule 9: no layer beneath, no CNXMAN */

	cn = (struct vms_cnxman *)exec_zalloc(sizeof(*cn));
	if (cn == NULL)
		return (int)SS__INSFMEM;
	cn->cl = cl;

	(void)cnxman_club_init(cl);

	cnxman_ops_bind(cn);
	cnxman_jops_bind(cn);
	cnxman_vc_sysap_bind(cn);
	cnxman_mscp_sysap_bind(cn);

	cnxman_join_init(&cn->join, cl, &cn->ops, &cn->jops);
	cnxman_barrier_init(&cn->barrier, cl, &cn->ops);
	cnxman_coord_init(&cn->coord, cl, &cn->ops);
	cnxman_recnx_init(&cn->recnx, cl, &cn->ops);
	cnxman_join_set_barrier(&cn->join, &cn->barrier);
	/* No DLM arm in P3 (vms_cnxman_barrier_fsm.h / _coord_fsm.h: NULL is a
	 * real VMS configuration, a node with no distributed locking still
	 * joins) -- FC-P4.x installs one later via cnxman_set_dlm(). */

	/*
	 * E31: this node's own identity, as read from real executive state.
	 * Nothing here is asserted that the executive does not hold: model/
	 * version/params/conndata/dir_descriptor all stay `_valid = 0` --
	 * every one of those fields is an operator-reserved or lab-pinned
	 * decision (E31, the LOCKDIRWT/params offsets, E24's directory
	 * descriptor), never this glue's default to invent.
	 */
	memset(&cfg, 0, sizeof(cfg));
	cnxman_join_set_cfg(&cn->join, &cfg);

	status = (int)scs_sysap_listen(cl->scs, cnxman_join_name_vaxcluster,
				       &cn->vc_sysap, CNXMAN_VC_CREDITS);
	if (status != (int)SS__NORMAL) {
		exec_free(cn);
		return status;
	}
	status = (int)scs_sysap_listen(cl->scs, cnxman_join_name_disk_cl_drvr,
				       &cn->mscp_sysap, CNXMAN_MSCP_CREDITS);
	if (status != (int)SS__NORMAL) {
		(void)scs_sysap_unlisten(cl->scs, cnxman_join_name_vaxcluster);
		exec_free(cn);
		return status;
	}

	(void)cf_set_work_handler(cl->fork, CF_OWNER_CNXMAN, cnxman_work_handler,
				  cn);

	cl->cnxman = cn;
	cnxman_recnx_start(&cn->recnx);

	cnxman_start_join_or_wait(cn);
	return (int)SS__NORMAL;
}

void vms_cnxman_stop(struct vms_cluster *cl)
{
	struct vms_cnxman *cn;
	struct cnxman_recnx_rec rec;

	if (cl == NULL || cl->cnxman == NULL)
		return;
	cn = cl->cnxman;

	/* p. 7-29: emit the last gasp before anything else tears down, so the
	 * survivors remove this node at once instead of waiting out the
	 * reconnect period. */
	if (cnxman_recnx_shutdown(&cn->recnx, &rec, 1) == 1u &&
	    rec.action == (uint8_t)CNXMAN_CSB_ACT_LAST_GASP && cl->pe != NULL) {
		/* The datagram itself is a PORT-level frame (wire spec
		 * SS4(O.30)); FC-P0.9's vms_pe.h owns building and sending
		 * it. No last-gasp builder is wired into this glue yet -- an
		 * honest gap, not a fabricated send. */
	}

	if (cl->scs != NULL) {
		(void)scs_sysap_unlisten(cl->scs, cnxman_join_name_disk_cl_drvr);
		(void)scs_sysap_unlisten(cl->scs, cnxman_join_name_vaxcluster);
	}
	if (cl->fork != NULL) {
		cf_timer_cancel(cl->fork, CF_OWNER_CNXMAN,
				(uint32_t)CNXMAN_TIMER_RECNX, 0u);
		cf_timer_cancel(cl->fork, CF_OWNER_CNXMAN,
				(uint32_t)CNXMAN_TIMER_JOIN, 0u);
		cf_timer_cancel(cl->fork, CF_OWNER_CNXMAN,
				(uint32_t)CNXMAN_TIMER_BARRIER, 0u);
		cf_timer_cancel(cl->fork, CF_OWNER_CNXMAN,
				(uint32_t)CNXMAN_TIMER_COORD, 0u);
		(void)cf_set_work_handler(cl->fork, CF_OWNER_CNXMAN, NULL, NULL);
	}

	cl->cnxman = NULL;
	exec_free(cn);
}

/* ==========================================================================
 * 10. CLUB / CSB query -- CLUSTER_DIAG_CSB, SHOW CLUSTER, $GETSYI (vms_cnxman.h SS6)
 * ========================================================================== */

int cnxman_get_club(struct vms_cluster *cl, struct vms_club_view *out)
{
	if (cl == NULL || out == NULL)
		return (int)SS__BADPARAM;
	memset(out, 0, sizeof(*out));
	if (cl->cnxman == NULL)
		return (int)SS__NOSUCHDEV;

	vms_cluster_fork_enter(cl);
	cnxman_club_project(&cl->club, cl->state, out);
	vms_cluster_fork_leave(cl);
	return (int)SS__NORMAL;
}

int cnxman_get_csb(struct vms_cluster *cl, uint32_t index,
		   struct vms_csb_view *out)
{
	struct vms_csb *csb;

	if (cl == NULL || out == NULL)
		return (int)SS__BADPARAM;
	memset(out, 0, sizeof(*out));
	if (cl->cnxman == NULL)
		return (int)SS__NOSUCHDEV;

	vms_cluster_fork_enter(cl);
	csb = cnxman_club_csb_at(&cl->club, index);
	if (csb != NULL)
		cnxman_csb_project(csb, out);
	vms_cluster_fork_leave(cl);
	return csb != NULL ? (int)SS__NORMAL : (int)SS__NOSUCHDEV;
}

int cnxman_find_csb(struct vms_cluster *cl, vms_csid_t csid,
		    struct vms_csb_view *out)
{
	struct vms_csb *csb;

	if (cl == NULL || out == NULL)
		return (int)SS__BADPARAM;
	memset(out, 0, sizeof(*out));
	if (cl->cnxman == NULL)
		return (int)SS__NOSUCHDEV;

	vms_cluster_fork_enter(cl);
	csb = cnxman_club_find_csid(&cl->club, csid);
	if (csb != NULL)
		cnxman_csb_project(csb, out);
	vms_cluster_fork_leave(cl);
	return csb != NULL ? (int)SS__NORMAL : (int)SS__NOSUCHDEV;
}

int cnxman_get_transition(struct vms_cluster *cl, struct cnxman_transition *out)
{
	struct vms_cnxman *cn;
	int rc;

	if (cl == NULL || out == NULL)
		return (int)SS__BADPARAM;
	memset(out, 0, sizeof(*out));
	if (cl->cnxman == NULL)
		return (int)SS__NOSUCHDEV;
	cn = cl->cnxman;

	vms_cluster_fork_enter(cl);
	rc = cnxman_coord_transition(&cn->coord, out);
	if (rc != 0)
		rc = cnxman_barrier_transition(&cn->barrier, out);
	vms_cluster_fork_leave(cl);
	return rc == 0 ? (int)SS__NORMAL : (int)SS__NOSUCHDEV;
}

/* ==========================================================================
 * 11. The DLM's wire arm (vms_cnxman.h SS5)
 * ========================================================================== */

void cnxman_set_dlm(struct vms_cluster *cl, const struct dlm_scs_role_ops *ops)
{
	struct vms_cnxman *cn;

	if (cl == NULL || cl->cnxman == NULL)
		return;
	cn = cl->cnxman;

	vms_cluster_fork_enter(cl);
	cnxman_barrier_set_dlm(&cn->barrier, ops);
	cnxman_coord_set_dlm(&cn->coord, ops);
	vms_cluster_fork_leave(cl);
}

/* ==========================================================================
 * 11b. The disk class driver's hook (vms_cnxman.h SS5b, FC-P7.1)
 * ========================================================================== */

void cnxman_set_disk_client(struct vms_cluster *cl,
			    const struct cnxman_disk_client_ops *ops)
{
	struct vms_cnxman *cn;

	if (cl == NULL || cl->cnxman == NULL)
		return;
	cn = cl->cnxman;

	vms_cluster_fork_enter(cl);
	if (ops == NULL) {
		memset(&cn->disk_client, 0, sizeof(cn->disk_client));
		cn->disk_client_set = 0u;
	} else {
		cn->disk_client = *ops;
		cn->disk_client_set = 1u;
	}
	vms_cluster_fork_leave(cl);
}

int cnxman_disk_client_connect(struct vms_cluster *cl, vms_scs_sysid_t dst,
			       vms_conid_t *out_conid)
{
	if (cl == NULL || out_conid == NULL)
		return (int)SS__BADPARAM;
	if (cl->cnxman == NULL || cl->scs == NULL)
		return (int)SS__NOSUCHDEV;
	/* The two names are vms_cnxman_join_fsm.c's, which is where the ONE
	 * spelling of each lives; no caller re-types them. */
	return scs_connect(cl->scs, cnxman_join_name_disk_cl_drvr,
			   cnxman_join_name_mscp_disk, dst, out_conid);
}

/* ==========================================================================
 * 12. $SETCLUEVT (SS7) -- registration and process-death safety
 * ========================================================================== */

int vms_cnxman_cluevt_set(struct vms_cluster *cl, void *proc,
			  uint32_t event_mask, uint64_t astadr,
			  uint64_t astprm)
{
	struct vms_cnxman *cn;

	if (cl == NULL)
		return (int)SS__BADPARAM;
	if (cl->cnxman == NULL)
		return (int)SS__NOSUCHDEV;
	cn = cl->cnxman;

	vms_cluster_fork_enter(cl);
	if (event_mask == 0u || proc == NULL || astadr == 0u) {
		cn->cluevt_proc = NULL;
		cn->cluevt_mask = 0u;
		cn->cluevt_astadr = 0u;
		cn->cluevt_astprm = 0u;
	} else {
		cn->cluevt_proc = proc;
		cn->cluevt_mask = event_mask;
		cn->cluevt_astadr = astadr;
		cn->cluevt_astprm = astprm;
	}
	vms_cluster_fork_leave(cl);
	return (int)SS__NORMAL;
}

void vms_cnxman_proc_gone(struct vms_cluster *cl, void *proc)
{
	struct vms_cnxman *cn;

	if (cl == NULL || cl->cnxman == NULL || proc == NULL)
		return;
	cn = cl->cnxman;

	vms_cluster_fork_enter(cl);
	if (cn->cluevt_proc == proc) {
		cn->cluevt_proc = NULL;
		cn->cluevt_mask = 0u;
	}
	vms_cluster_fork_leave(cl);
}
