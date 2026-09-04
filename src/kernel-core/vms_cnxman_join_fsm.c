/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman_join_fsm.c - the JOIN (FC-P3.3).
 *
 * The choreography, its grounding step by step, and the five things this file
 * refuses to invent are all in vms_cnxman_join_fsm.h. Read it first; this file
 * is the behaviour.
 *
 * READ THE TABLE, NOT THE PROSE. join_table[][] below IS the specification of
 * this machine: one cell per [state][event], one small handler per edge, and
 * every empty cell an event the evidence does not connect to that state --
 * ignored and COUNTED, never guessed.
 *
 * THREE STRUCTURAL SAFETIES, because the failure mode here is "never admitted,
 * or admitted on a lie":
 *
 *   1. LOOKUP BEFORE CONNECT IS UNREACHABLE TO VIOLATE. Each of the two
 *      connects this FSM makes is issued from ONE place, reached only after
 *      that name's own directory answer really arrived. Firing the MSCP$DISK
 *      connect before resolving the name froze a real member's recv_ack and
 *      regressed OVMX below NEW to blank status (spec sec 4(L)).
 *
 *   2. NOTHING IS ANSWERED EXCEPT THROUGH THE ALLOWLIST. Every response path
 *      checks vms_cm_allow_table() for the ACTION and the RECIPE it is about
 *      to use. Answering an ungrounded pair crashed two real VAXes.
 *
 *   3. NO RAW WIRE OFFSET, AND NO ENVELOPE BYTE. Every field read or written
 *      goes through vms_cluster_codec_cm.h / vms_cluster_codec_mscp.h, and
 *      body[0:8] is written by exactly one function -- cnxman_envelope_stamp()
 *      -- from the destination CSB's real dialogue counters, which this file
 *      advances through named CSB operations and never by hand.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 * PURE TU: no seam call, no allocation, no clock but ops->now_ms.
 */

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_barrier_fsm.h"
#include "vms_cnxman_join_fsm.h"
#include "vms_cnxman_diag.h"
#include "vms_cluster_codec_cm.h"
#include "vms_cluster_codec_mscp.h"
#include "vms_mscp_cl_fsm.h"

/* ==========================================================================
 * The published SYSAP names
 *
 * Blank-padded to 16 bytes, because NUL padding is a DIFFERENT name to a real
 * VAX (vms_scs_dir.h says the same thing at its own two names).
 * ========================================================================== */
const uint8_t cnxman_join_name_vaxcluster[VMS_SCS_PROCNAME_LEN] = {
	'V', 'M', 'S', '$', 'V', 'A', 'X', 'c', 'l', 'u', 's', 't', 'e', 'r',
	' ', ' '
};
const uint8_t cnxman_join_name_mscp_disk[VMS_SCS_PROCNAME_LEN] = {
	'M', 'S', 'C', 'P', '$', 'D', 'I', 'S', 'K', ' ', ' ', ' ', ' ', ' ',
	' ', ' '
};
const uint8_t cnxman_join_name_disk_cl_drvr[VMS_SCS_PROCNAME_LEN] = {
	'V', 'M', 'S', '$', 'D', 'I', 'S', 'K', '_', 'C', 'L', '_', 'D', 'R',
	'V', 'R'
};

/* ==========================================================================
 * Small shared helpers. This TU calls no library (a pure TU builds on the
 * host too, where the substrate's memset is not in scope).
 * ========================================================================== */

static void join_bzero(void *p, uint32_t n)
{
	uint8_t *o = (uint8_t *)p;
	uint32_t i;

	for (i = 0; i < n; i++)
		o[i] = 0u;
}

static void join_bcopy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		dst[i] = src[i];
}

static int join_name_eq(const uint8_t *a, const uint8_t *b)
{
	uint32_t i;

	for (i = 0; i < VMS_SCS_PROCNAME_LEN; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}

static void join_log(const struct cnxman_join *j, const char *msg)
{
	if (j->ops != NULL && j->ops->log != NULL)
		j->ops->log(j->ops->ctx, msg);
}

/* ==========================================================================
 * The E69 transition ring -- OBSERVABILITY ONLY
 *
 * Three call sites in this file record into it (the [state][event] dispatcher,
 * the CM/MSCP emit paths, and the entry points that decide WITHOUT the table),
 * and every one of them is a store with no return value: nothing below reads
 * the ring, branches on it, or fails because of it. The clock is this FSM's
 * OWN injected one -- a pure TU may not read a clock, and the ring may not
 * introduce the one exception (gate RULE4).
 * ========================================================================== */

static uint32_t join_now_ms(const struct cnxman_join *j)
{
	if (j->ops != NULL && j->ops->now_ms != NULL)
		return j->ops->now_ms(j->ops->ctx);
	return 0u;   /* honest: no clock is bound, not "time zero" */
}

/* An EMIT record for a body this node built. `cat`/`op` are read back out of
 * the bytes really handed to SCS by the caller -- never the builder's intent. */
static void join_diag_emit(const struct cnxman_join *j, uint8_t cat, uint8_t op,
			   enum cnxman_diag_gate gate, int32_t rc,
			   uint32_t conid)
{
	cnxman_diag_emit(j->diag, join_now_ms(j), j->state, cat, op,
			 (uint8_t)gate, rc, conid);
}

/* An ARRIVAL record: a real fact reached an entry point and did NOT reach the
 * [state][event] table -- with the reason it stopped. */
static void join_diag_arrival(const struct cnxman_join *j, uint8_t event,
			      enum cnxman_diag_reason reason, int32_t rc,
			      uint32_t aux)
{
	cnxman_diag_arrival(j->diag, join_now_ms(j), j->state, event,
			    (uint8_t)reason, rc, aux);
}

/* The (category, opcode) an inbound frame REALLY carried, packed into the
 * ring's one `aux` longword: category in bits 8-15, opcode in bits 0-7. Both
 * come from the parsed envelope -- the frame's own bytes, never a guess about
 * what it should have been. */
static uint32_t join_diag_catop(const struct vms_cm_envelope *env)
{
	return ((uint32_t)env->category << 8) | (uint32_t)env->opcode;
}

static const char *const join_state_names[CNXMAN_JOIN_STATE__COUNT] = {
	"IDLE", "DIR ROUND", "MSCP CONNECT", "VC CONNECT", "ADVERTISE",
	"ADMIT", "BARRIER", "MEMBER", "FAILED"
};

const char *cnxman_join_state_name(enum cnxman_join_state s)
{
	if ((unsigned)s >= (unsigned)CNXMAN_JOIN_STATE__COUNT)
		return "?";
	return join_state_names[s];
}

const char *cnxman_join_failure_name(enum cnxman_join_failure f)
{
	switch (f) {
	case CNXMAN_JOIN_FAIL_NONE:      return "none";
	case CNXMAN_JOIN_FAIL_NO_TARGET: return "no member to join through";
	case CNXMAN_JOIN_FAIL_CONNECT:   return "connect refused locally";
	case CNXMAN_JOIN_FAIL_REJECTED:  return "connect rejected by the peer";
	case CNXMAN_JOIN_FAIL_PATHLOST:  return "path lost";
	case CNXMAN_JOIN_FAIL_ABSENT:    return "SYSAP not present on the member";
	case CNXMAN_JOIN_FAIL_SEND:      return "message could not be sent";
	case CNXMAN_JOIN_FAIL_CODEC:     return "codec refused to build";
	default:                         return "?";
	}
}

/* ==========================================================================
 * State movement
 * ========================================================================== */

static void join_goto(struct cnxman_join *j, enum cnxman_join_state s)
{
	j->state = (uint8_t)s;
}

static void join_fail(struct cnxman_join *j, enum cnxman_join_failure why,
		      const char *msg)
{
	j->failure = (uint8_t)why;
	join_goto(j, CNXMAN_JOIN_FAILED);
	join_log(j, msg);
}

/*
 * ONE join, ONE watchdog, so the key is the constant 0 and re-arming MOVES the
 * same timer identity rather than leaving a stale deadline behind for every
 * state the drive has passed through.
 */
#define JOIN_WATCH_KEY 0u

static void join_arm_watch(struct cnxman_join *j)
{
	if (j->ops != NULL && j->ops->arm_timer != NULL)
		j->ops->arm_timer(j->ops->ctx, CNXMAN_TIMER_JOIN,
				  JOIN_WATCH_KEY, CNXMAN_JOIN_WATCH_MS);
}

/* ==========================================================================
 * The destination CSB -- the only source of an outbound body's envelope
 *
 * No CSB for the member means this FSM ORIGINATES NOTHING, which is the honest
 * outcome (INV-6), not a zero-filled frame.
 * ========================================================================== */

static struct vms_csb *join_target_csb(struct cnxman_join *j)
{
	if (j->cl == NULL || !j->target_valid || j->target_csb < 0)
		return NULL;
	return cnxman_club_csb_at(&j->cl->club, (uint32_t)j->target_csb);
}

/* ==========================================================================
 * Sending
 *
 * ONE path out for every `VMS$VAXcluster` body: advance the dialogue counter,
 * stamp body[0:8] from the CSB, hand the 132 bytes to SCS. A build that failed
 * never reaches here, and a send that failed is COUNTED and named.
 * ========================================================================== */

/*
 * WHICH precondition of an origination is unmet. Split out of join_emit_cm()'s
 * one compound test because "the burst did not go out" has three completely
 * different diagnoses -- no CSB to stamp an envelope from, no OPEN
 * VMS$VAXcluster connection to put it on, or no send op bound at all -- and
 * E69 exists to tell them apart on a live cluster. Returns
 * CNXMAN_DIAG_G_SENT when every precondition holds.
 */
static enum cnxman_diag_gate join_emit_gate(const struct cnxman_join *j,
					    const struct vms_csb *csb)
{
	if (csb == NULL)
		return CNXMAN_DIAG_G_NO_CSB;
	if (!j->cm_open)
		return CNXMAN_DIAG_G_NO_CONN;
	if (j->jops == NULL || j->jops->send_msg == NULL)
		return CNXMAN_DIAG_G_NO_OPS;
	return CNXMAN_DIAG_G_SENT;
}

/*
 * The (category, opcode) of the body sitting in `scratch`, READ BACK OUT of
 * the bytes themselves through the codec (vms_cm_body_kind) rather than taken
 * from whichever builder ran. INV-6: the ring reports what is being sent, not
 * what the caller meant to send -- a builder that wrote the wrong opcode is
 * precisely the class of defect this instrument has to be able to show. A body
 * the codec cannot read leaves both zero, which the renderer prints verbatim.
 */
static void join_body_kind(const struct cnxman_join *j, uint8_t *cat,
			   uint8_t *op)
{
	*cat = 0u;
	*op  = 0u;
	(void)vms_cm_body_kind(j->scratch, (uint32_t)sizeof(j->scratch),
			       cat, op);
}

static int join_emit_cm(struct cnxman_join *j, int is_response)
{
	struct vms_csb *csb = join_target_csb(j);
	enum cnxman_diag_gate gate = join_emit_gate(j, csb);
	uint8_t cat, op;
	int rc;

	join_body_kind(j, &cat, &op);

	if (gate != CNXMAN_DIAG_G_SENT) {
		j->send_failures++;
		join_diag_emit(j, cat, op, gate, 0, j->cm_conid);
		return -1;
	}

	/*
	 * The send-msg# is assigned when the message is CREATED, before it is
	 * handed down -- which is what makes it strictly monotonic per sender
	 * (spec sec 4(j)) even when SCS holds the message in Credit Wait and
	 * transmits it later with the same bytes. A message SCS refuses
	 * outright therefore BURNS its number; that leaves a gap, never a
	 * repeat or a decrement, and the refusal is counted below.
	 */
	cnxman_csb_dialogue_sent(csb);
	cnxman_envelope_stamp(csb, j->scratch, is_response);

	rc = j->jops->send_msg(j->jops->ctx, j->cm_conid, j->scratch,
			       VMS_CM_BODY_LEN);
	if (rc != 0) {
		j->send_failures++;
		join_diag_emit(j, cat, op, CNXMAN_DIAG_G_REFUSED, (int32_t)rc,
			       j->cm_conid);
		return -1;
	}
	join_diag_emit(j, cat, op, CNXMAN_DIAG_G_SENT, 0, j->cm_conid);
	return 0;
}

/* A codec refusal and a send refusal are different facts and are counted as
 * such; a codec refusal ends the join honestly rather than silently. There is
 * no body when the codec refuses, so the ring records the refusal with both
 * wire bytes explicitly zero -- an omission, not a message. */
static int join_build_failed(struct cnxman_join *j, vms_codec_status_t st)
{
	if (st == VMS_CODEC_OK)
		return 0;
	j->codec_failures++;
	join_diag_emit(j, 0u, 0u, CNXMAN_DIAG_G_CODEC, (int32_t)st,
		       j->cm_conid);
	join_fail(j, CNXMAN_JOIN_FAIL_CODEC,
		  "%CNXMAN, could not build a VMS$VAXcluster message");
	return 1;
}

/* ==========================================================================
 * This node's own node-parameter block
 *
 * Every byte from cnxman_join_cfg, i.e. from what the glue read out of real
 * executive state. Nothing supplied is an explicit zero and a COUNTED
 * omission -- never a captured "V7.3" and never the observed 0x10/0x01 pair
 * whose meaning nobody knows (spec sec 4(j)).
 * ========================================================================== */

static void join_own_params(struct cnxman_join *j,
			    struct vms_cm_node_params *out)
{
	join_bzero(out, (uint32_t)sizeof(*out));

	if (j->cfg.params_valid) {
		out->param_f1 = j->cfg.param_f1;
		out->param_f2 = j->cfg.param_f2;
	} else {
		j->node_params_omitted++;
	}
	if (j->cfg.version_valid)
		join_bcopy(out->version, j->cfg.version, VMS_CM_VERSION_LEN);
	else
		j->version_omitted++;
}

/* ==========================================================================
 * The three originations of the joiner's own burst (spec sec 4(o))
 * ========================================================================== */

static void join_send_model(struct cnxman_join *j)
{
	vms_codec_status_t st;
	const uint8_t *name = j->cfg.model_valid ? j->cfg.model : NULL;
	uint8_t len = j->cfg.model_valid ? j->cfg.model_len : 0u;

	if (!j->cfg.model_valid)
		j->model_omitted++;

	st = vms_cm_model_build(name, len, j->scratch,
				(uint32_t)sizeof(j->scratch), NULL);
	if (join_build_failed(j, st))
		return;
	if (join_emit_cm(j, 0) == 0)
		j->model_sent++;
}

/*
 * LOCKDIRWT, said out loud on every PARAMS this node sends.
 *
 * Book D-DLM-1 has OVMX advertise 0. vms_cm_params_build() writes only
 * grounded placements, so a 0 and "not written" are the same bytes -- which
 * is a coincidence and not the field being placed (plan row FC-P3.2 owns the
 * offset). A node configured with a NONZERO LOCKDIRWT genuinely cannot
 * advertise it, and this says so loudly: silently understating a directory
 * weight would make the cluster route directory duty away from a node that
 * asked for it.
 */
static void join_note_lockdirwt(struct cnxman_join *j)
{
	j->lockdirwt_unpinned++;
	if (j->cl != NULL && j->cl->params.lockdirwt != 0u &&
	    !j->lockdirwt_unrepresentable) {
		j->lockdirwt_unrepresentable = 1u;
		join_log(j, "%CNXMAN, LOCKDIRWT is nonzero but its wire offset "
			    "is not pinned: it is NOT being advertised");
	}
}

static void join_send_params(struct cnxman_join *j)
{
	struct vms_cm_node_params own;
	vms_codec_status_t st;
	uint16_t votes = 0u;

	/* VOTES from the REAL SYSGEN parameters this node booted with, never a
	 * default (spec sec 4(j) pinned body[22:24] byte-exact across four
	 * configured values; 0 is a legitimate one). */
	if (j->cl != NULL)
		votes = j->cl->params.votes;

	join_own_params(j, &own);
	join_note_lockdirwt(j);

	st = vms_cm_params_build(votes, &own, j->scratch,
				 (uint32_t)sizeof(j->scratch), NULL);
	if (join_build_failed(j, st))
		return;
	if (join_emit_cm(j, 0) == 0)
		j->params_sent++;
}

static void join_send_config(struct cnxman_join *j)
{
	vms_codec_status_t st;

	st = vms_cm_config_build(j->scratch, (uint32_t)sizeof(j->scratch),
				 NULL);
	if (join_build_failed(j, st))
		return;
	if (join_emit_cm(j, 0) == 0)
		j->config_sent++;
}

/* ==========================================================================
 * The allowlist gate -- safety 2
 * ========================================================================== */

static int join_recipe_allowed(uint8_t category, uint8_t opcode,
			       uint16_t recipe)
{
	const struct vms_wire_allow_entry *e;

	e = vms_wire_allow_find(vms_cm_allow_table(),
				(uint8_t)VMS_SYSAP_VMS_VAXCLUSTER,
				category, opcode);
	return e != NULL && e->action == (uint8_t)VMS_WIRE_ACT_RESPOND &&
	       e->recipe == recipe;
}

/* ==========================================================================
 * One dispatched event
 * ========================================================================== */

struct join_ev {
	/* frame events */
	const uint8_t         *frame;
	uint32_t               len;
	struct vms_frame_info  fi;
	struct vms_cm_envelope env;
	vms_csid_t             from_csid;
	int                    from_valid;

	/* connection events */
	vms_conid_t conid;
	uint32_t    reason;

	/* directory result */
	const uint8_t  *name;
	vms_scs_sysid_t from_sysid;
	int             present;

	/* the assignment the cluster made */
	vms_csid_t csid;
};

typedef enum cnxman_join_rx (*join_handler_t)(struct cnxman_join *,
					      const struct join_ev *);

/* ==========================================================================
 * Starting: pick the member, declare our directory descriptor, run our own
 * SCS$DIRECTORY client round
 * ========================================================================== */

/*
 * Choose the member to join through. Book pp. 7-37/7-38 (correction D7) ranks
 * by VAXcluster protocol level, then ECO level, then "the CSB nearest the end
 * of the CLUB's CSB queue". Neither level has an isolated wire offset, so only
 * the LAST rule is evaluable from real state -- and it is evaluated here, on
 * the CLUB's own table, with the two unusable rules COUNTED so the gap shows
 * up in the diagnostics instead of on a real cluster.
 */
static int join_select_target(struct cnxman_join *j)
{
	struct vms_club *club;
	struct vms_csb *local;
	uint32_t i;

	j->target_valid = 0u;
	j->target_csb = -1;
	if (j->cl == NULL)
		return -1;

	club = &j->cl->club;
	local = cnxman_club_local(club);

	for (i = club->n_csb; i > 0u; i--) {
		struct vms_csb *c = cnxman_club_csb_at(club, i - 1u);

		if (c == NULL || c == local || !c->sysid_valid)
			continue;
		j->target_sysid = c->sysid;
		j->target_csb = (int32_t)(i - 1u);
		j->target_valid = 1u;
		j->target_level_unpinned++;
		j->member_count_ungated++;
		return 0;
	}
	return -1;
}

/* Declare what a directory HIT on our own VMS$VAXcluster name carries
 * (integration note E24). With nothing grounded supplied we declare NOTHING
 * and the directory service falls back to its honest name-echo. */
static void join_declare_dir_data(struct cnxman_join *j)
{
	if (!j->cfg.dir_descriptor_valid) {
		j->dir_descriptor_omitted++;
		join_log(j, "%CNXMAN, VMS$VAXcluster directory descriptor is "
			    "not grounded: answering with the registered name");
		return;
	}
	if (j->jops != NULL && j->jops->set_dir_data != NULL)
		(void)j->jops->set_dir_data(j->jops->ctx,
					    cnxman_join_name_vaxcluster,
					    j->cfg.dir_descriptor);
}

/*
 * Steps 1 and 2 are ONE act here, and that is the module boundary rather than
 * a shortcut: p. 2-51 makes the directory connection the POLLER's, opened when
 * it has something to ask and closed when nothing is outstanding, and
 * vms_scs_dir.h implements exactly that transient round. So this FSM asks, and
 * the act of asking is what opens our own SCS$DIRECTORY connection -- which is
 * spec sec 4(L)(a)+(b), "open its own SCS$DIRECTORY CLIENT connection ... look
 * up each SYSAP on the member as a client before connecting to it".
 */
static uint32_t join_send_lookups(struct cnxman_join *j)
{
	uint32_t issued = 0u;

	if (j->jops == NULL || j->jops->dir_inquire == NULL)
		return 0u;
	/* Only names this member has not already ANSWERED about. A name it
	 * answered "NOT PRESENT HERE" is answered; re-asking it would be
	 * refusing to believe a real answer. */
	if ((j->lookups_answered & CNXMAN_JOIN_L_MSCP_DISK) == 0u &&
	    j->jops->dir_inquire(j->jops->ctx, j->target_sysid,
				 cnxman_join_name_mscp_disk) == 0)
		issued++;
	if ((j->lookups_answered & CNXMAN_JOIN_L_VAXCLUSTER) == 0u &&
	    j->jops->dir_inquire(j->jops->ctx, j->target_sysid,
				 cnxman_join_name_vaxcluster) == 0)
		issued++;
	j->lookups_sent += issued;
	return issued;
}

static enum cnxman_join_rx join_h_start(struct cnxman_join *j,
					const struct join_ev *e)
{
	(void)e;

	if (join_select_target(j) != 0) {
		join_fail(j, CNXMAN_JOIN_FAIL_NO_TARGET,
			  "%CNXMAN, no connection manager to join through");
		return CNXMAN_JOIN_RX_CONSUMED;
	}
	join_declare_dir_data(j);

	if (join_send_lookups(j) == 0u) {
		join_fail(j, CNXMAN_JOIN_FAIL_CONNECT,
			  "%CNXMAN, could not open an SCS$DIRECTORY "
			  "connection to the cluster");
		return CNXMAN_JOIN_RX_CONSUMED;
	}
	j->joins_started++;
	join_goto(j, CNXMAN_JOIN_DIR_ROUND);
	join_arm_watch(j);
	return CNXMAN_JOIN_RX_CONSUMED;
}

/* ==========================================================================
 * Connecting, once -- and only once -- the name has really been resolved
 * ========================================================================== */

static int join_open(struct cnxman_join *j, const uint8_t *local_name,
		     const uint8_t *remote_name, const uint8_t *conndata,
		     uint16_t credits, vms_conid_t *out)
{
	if (j->jops == NULL || j->jops->connect == NULL)
		return -1;
	return j->jops->connect(j->jops->ctx, j->target_sysid, local_name,
				remote_name, conndata, credits, out);
}

/* Forward: the burst the moment the CM connection is OPEN, whichever side
 * opened it (defined with the other step-5 handlers, below). */
static void join_cm_advertise(struct cnxman_join *j);

/*
 * Step 4: the VMS$VAXcluster VC. There is exactly ONE such connection per pair
 * of systems and either side may open it (E67; spec sec 4(L)(1) describes the
 * leg the reference joiner won, and the same capture shows it accepting the
 * other). If the member's inbound connect already arrived and this join
 * adopted it, opening a second one here would give the pair two -- so the
 * adopted one IS this step's outcome and the burst goes out on it now.
 *
 * The 16-byte connect data is the Connection Managers' version handshake
 * (p. 2-25) and is the caller's or nothing -- see "REFUSES TO INVENT", C.
 */
static void join_open_cm(struct cnxman_join *j)
{
	const uint8_t *cd = j->cfg.conndata_valid ? j->cfg.conndata : NULL;

	if (j->cm_open) {
		join_cm_advertise(j);
		return;
	}
	if (cd == NULL) {
		j->conndata_omitted++;
		join_log(j, "%CNXMAN, no SCA connect data configured: the "
			    "VMS$VAXcluster version field goes out empty");
	}
	if (join_open(j, cnxman_join_name_vaxcluster,
		      cnxman_join_name_vaxcluster, cd,
		      CNXMAN_JOIN_CM_CREDITS, &j->cm_conid) != 0) {
		join_fail(j, CNXMAN_JOIN_FAIL_CONNECT,
			  "%CNXMAN, could not open the VMS$VAXcluster "
			  "connection");
		return;
	}
	join_goto(j, CNXMAN_JOIN_VC_CONNECT);
	join_arm_watch(j);
}

/* Step 3: the disk-client connection, reachable only from a real HIT. */
static void join_open_mscp(struct cnxman_join *j)
{
	if (join_open(j, cnxman_join_name_disk_cl_drvr,
		      cnxman_join_name_mscp_disk, NULL,
		      CNXMAN_JOIN_MSCP_CREDITS, &j->mscp_conid) != 0) {
		join_fail(j, CNXMAN_JOIN_FAIL_CONNECT,
			  "%CNXMAN, could not open an MSCP$DISK connection");
		return;
	}
	join_goto(j, CNXMAN_JOIN_MSCP_CONNECT);
	join_arm_watch(j);
}

/*
 * THE DISK-CLIENT CONNECTION IS NOT A MEMBERSHIP PREREQUISITE (E68).
 *
 * THE WALL. On the live 2-node VAX cluster (join-e67refire, 2026-09-04) this
 * node resolved both names, issued its MSCP$DISK connect at t+14.9372, and the
 * member answered REJECT_REQUEST 0.2 ms later. SCS closed that CDT with
 * SCS_CLOSE_REJECTED, the glue reported it as a close, and this file failed the
 * WHOLE JOIN with PATHLOST -- "lost a connection needed to join the cluster".
 * [FAILED] is an empty table row and cnxman_join_drive() will not restart a
 * non-IDLE join, so when BOTH members opened their own VMS$VAXcluster
 * connection to this node 1.2 s later and each sent its cat-0x01 op-0x01
 * parameters on it, the CNXMAN_EV_CM_ACCEPTED for each was counted and dropped,
 * not one CONFIG-category frame ever went back, and both CSBs for this node sat
 * at votes 0 until %CNXMAN timed them out. The pcap shows ZERO 204-byte CM
 * frames from this node in the whole 1600 s run.
 *
 * WHY THE FAILURE WAS WRONG, on three independent grounds:
 *
 *   1. THIS FILE ALREADY SAYS SO for the neighbouring fact. A member that hosts
 *      no MSCP$DISK at all is "a real configuration, not a failure"
 *      (join_lookups_complete below): the join counts `mscp_absent`, marks the
 *      walk done and goes straight on to the VMS$VAXcluster step. A member that
 *      HOSTS MSCP$DISK but refuses this node's disk-client connection leaves
 *      the join in exactly the same position -- there is no discovery to do --
 *      so it cannot be fatal when the other is not.
 *
 *   2. THE REFERENCE JOIN MEASURES THE TWO AS INDEPENDENT
 *      (vax3-2to3-established-join-20260730, decoded this session). On its VAX2
 *      leg the joiner's VMS$VAXcluster connection reached OPEN at t+30.3690 and
 *      its MSCP$DISK connect to that same member only went out at t+30.8266 --
 *      0.46 s LATER. The membership connection is not downstream of the disk
 *      client on the real wire. The same capture also shows the reference
 *      JOINER itself answering REJECT_REQUEST to both members' inbound
 *      MSCP$DISK connects (t+29.8469 and t+30.3750) while its own join
 *      proceeds: a refused disk-client connect is ordinary traffic in this
 *      dialogue, not a verdict.
 *
 *   3. THE REJECT CARRIES NO VERDICT TO READ. Book p. 2-25 / correction D12
 *      makes a REJECT the Connection Managers' judgement on the 16-byte connect
 *      data -- and join_open_mscp() passes `conndata = NULL` on this connect, as
 *      the wire confirms. A connection that asserted no version identity cannot
 *      have had one refused.
 *
 * WHAT IS HONESTLY LOST, and counted: this node enumerates none of that
 * member's served units. It says so (`mscp_rejected` / `mscp_lost`) instead of
 * pretending to a walk it did not run -- and it does NOT retry, because the
 * member gave a real answer and re-asking would be refusing to believe it.
 */
static void join_disk_client_gone(struct cnxman_join *j)
{
	j->mscp_open = 0u;
	j->mscp_conid = 0u;      /* honest: this node holds no such connection */
	j->mscp_walk_done = 1u;  /* ... so there is no walk left to wait for   */

	/*
	 * MSCP_CONNECT is the one state whose entire purpose was to wait for
	 * this connection, so it is the one state the drive must be carried on
	 * from. From ADVERTISE onward the VMS$VAXcluster connection is already
	 * up and losing the disk client costs the join nothing (which is what
	 * the pre-E68 `mscp_walk_done` test already did); from DIR_ROUND the
	 * connect has not been issued and no Con.ID of ours can match.
	 */
	if (j->state == (uint8_t)CNXMAN_JOIN_MSCP_CONNECT)
		join_open_cm(j);
}

/*
 * Both inquiries are in. Two different answers mean two different things, and
 * conflating them would be a bug in either direction:
 *
 *   - no VMS$VAXcluster on the member: it runs no connection manager, so
 *     there is no cluster to join THROUGH it. Honest, named failure.
 *   - no MSCP$DISK on the member: a real configuration (MSCP_LOAD 0 -- the
 *     member serves no disks). There is then no disk-client discovery to do,
 *     and "send op 0x02 when your discovery has finished" (spec sec 4(o)) is
 *     satisfied immediately. Counted, not fatal.
 */
static void join_lookups_complete(struct cnxman_join *j)
{
	if ((j->lookups_hit & CNXMAN_JOIN_L_VAXCLUSTER) == 0u) {
		join_fail(j, CNXMAN_JOIN_FAIL_ABSENT,
			  "%CNXMAN, the member does not host VMS$VAXcluster: "
			  "it is not running a connection manager");
		return;
	}
	if ((j->lookups_hit & CNXMAN_JOIN_L_MSCP_DISK) != 0u) {
		join_open_mscp(j);
		return;
	}
	j->mscp_absent++;
	j->mscp_walk_done = 1u;
	join_open_cm(j);
}

static enum cnxman_join_rx join_h_dir_result(struct cnxman_join *j,
					     const struct join_ev *e)
{
	uint8_t bit;

	if (e->name == NULL) {
		j->ignored_events++;
		return CNXMAN_JOIN_RX_CONSUMED;
	}
	if (join_name_eq(e->name, cnxman_join_name_mscp_disk))
		bit = CNXMAN_JOIN_L_MSCP_DISK;
	else if (join_name_eq(e->name, cnxman_join_name_vaxcluster))
		bit = CNXMAN_JOIN_L_VAXCLUSTER;
	else {
		/* An answer about a name this join never asked about. Counted,
		 * never allowed to advance the drive. */
		j->ignored_events++;
		return CNXMAN_JOIN_RX_CONSUMED;
	}

	if (e->present) {
		j->lookups_hits++;
		j->lookups_hit |= bit;
	} else {
		j->lookups_misses++;
	}
	j->lookups_answered |= bit;

	if (j->lookups_answered == CNXMAN_JOIN_L_ALL)
		join_lookups_complete(j);
	return CNXMAN_JOIN_RX_CONSUMED;
}

static enum cnxman_join_rx join_h_mscp_opened(struct cnxman_join *j,
					      const struct join_ev *e)
{
	if (e->conid != j->mscp_conid) {
		j->ignored_events++;
		return CNXMAN_JOIN_RX_CONSUMED;
	}
	j->mscp_open = 1u;
	join_open_cm(j);
	return CNXMAN_JOIN_RX_CONSUMED;
}

/* ==========================================================================
 * The disk-client discovery walk (FC-P3.4 owns the protocol; this drives it)
 *
 * The MSCP command builder writes a whole 108-byte frame through a
 * `struct vms_mscp_link` covering abs [0,72). This FSM sends BODY-LEVEL, so
 * the link is ALL ZERO and only frame[72:108] is transmitted: SCS fills
 * abs 56-71 from the real CDT and the port fills abs 0-55 from the real
 * circuit (design sec 3.2.4 ruling E1). Not one byte of the zero prefix
 * reaches the wire -- which is exactly why passing zeros there is honest and
 * filling them in here would not be.
 * ========================================================================== */

/* The P.OPCD byte of the command in `mscp_frame`, read back through the MSCP
 * codec for the same reason join_body_kind() reads the CM pair back: the ring
 * reports the command really being sent. */
static uint8_t join_mscp_opcode(const struct cnxman_join *j)
{
	uint8_t op = 0u;

	(void)vms_mscp_read_opcode(j->mscp_frame,
				   (uint32_t)sizeof(j->mscp_frame), &op);
	return op;
}

static int join_mscp_emit(struct cnxman_join *j)
{
	uint8_t op = join_mscp_opcode(j);
	int rc;

	if (!j->mscp_open) {
		j->send_failures++;
		join_diag_emit(j, CNXMAN_DIAG_CAT_MSCP, op,
			       CNXMAN_DIAG_G_NO_CONN, 0, j->mscp_conid);
		return -1;
	}
	if (j->jops == NULL || j->jops->send_msg == NULL) {
		j->send_failures++;
		join_diag_emit(j, CNXMAN_DIAG_CAT_MSCP, op,
			       CNXMAN_DIAG_G_NO_OPS, 0, j->mscp_conid);
		return -1;
	}
	rc = j->jops->send_msg(j->jops->ctx, j->mscp_conid,
			       j->mscp_frame + VMS_OFF_SYSAP_BODY,
			       VMS_MSCP_CMD_BODY_LEN);
	if (rc != 0) {
		j->send_failures++;
		join_diag_emit(j, CNXMAN_DIAG_CAT_MSCP, op,
			       CNXMAN_DIAG_G_REFUSED, (int32_t)rc,
			       j->mscp_conid);
		return -1;
	}
	j->mscp_cmds_sent++;
	join_diag_emit(j, CNXMAN_DIAG_CAT_MSCP, op, CNXMAN_DIAG_G_SENT, 0,
		       j->mscp_conid);
	return 0;
}

static void join_mscp_send_scc(struct cnxman_join *j)
{
	struct vms_mscp_link link;
	vms_codec_status_t st;
	uint64_t now = 0u;

	join_bzero(&link, (uint32_t)sizeof(link));
	if (j->jops != NULL && j->jops->time_now != NULL)
		now = j->jops->time_now(j->jops->ctx);

	/*
	 * P.CNTF (controller flags) and P.HTMO (host timeout) are sec 6.16
	 * parameter fields with no OVMX policy behind them yet, so this
	 * discovery walk asks for no controller option and declares no host
	 * timeout: explicit zeros, which is honestly what "we have neither to
	 * declare" is. P.TIME is the executive's own clock, read through the
	 * seam by the glue (a pure TU may not read one -- gate RULE4).
	 */
	st = vms_mscp_cl_fsm_build_scc(&j->mscp, &link, 0u, 0u, now,
				       j->mscp_frame,
				       (uint32_t)sizeof(j->mscp_frame), NULL);
	if (st != VMS_CODEC_OK) {
		j->codec_failures++;
		join_diag_emit(j, CNXMAN_DIAG_CAT_MSCP, 0u,
			       CNXMAN_DIAG_G_CODEC, (int32_t)st, j->mscp_conid);
		return;
	}
	(void)join_mscp_emit(j);
}

static void join_mscp_send_gus(struct cnxman_join *j)
{
	struct vms_mscp_link link;
	vms_codec_status_t st;

	join_bzero(&link, (uint32_t)sizeof(link));
	st = vms_mscp_cl_fsm_build_gus(&j->mscp, &link, j->mscp_frame,
				       (uint32_t)sizeof(j->mscp_frame), NULL);
	if (st != VMS_CODEC_OK) {
		j->codec_failures++;
		join_diag_emit(j, CNXMAN_DIAG_CAT_MSCP, 0u,
			       CNXMAN_DIAG_G_CODEC, (int32_t)st, j->mscp_conid);
		return;
	}
	(void)join_mscp_emit(j);
}

/*
 * The walk is over. Spec sec 4(o) step 6: "then sends op 0x02". The rule is
 * NOT a delay -- "the joiner sends op 0x02 when its own disk-client discovery
 * is finished" -- so op-0x02 is emitted from HERE, on the peer's own
 * Unit-Offline terminator, and no timer is anywhere in this path.
 *
 * The transient directory connection is torn down by its own module when
 * nothing is outstanding (p. 2-51; vms_scs_dir.h's CLOSING state), which in
 * the clean 2-node reference happens as the round completes. This FSM does not
 * reach into it to force a teardown at a different instant than the module's
 * own rule -- an ordering difference between the two reference specimens that
 * belongs to FC-P2.3, not here.
 */
static void join_walk_complete(struct cnxman_join *j)
{
	j->mscp_walk_done = 1u;
	join_goto(j, CNXMAN_JOIN_ADMIT);
	join_send_config(j);
	join_arm_watch(j);
}

static void join_record_unit(struct cnxman_join *j,
			     const struct vms_mscp_cl_unit *u)
{
	if (j->units_found >= CNXMAN_JOIN_MAX_UNITS) {
		j->units_dropped++;
		return;
	}
	j->units[j->units_found] = *u;
	j->units_found++;
}

/* One END message. FC-P3.4 refuses an out-of-sequence or mis-correlated
 * answer for us, so a frame that is neither of the two this walk is waiting
 * for cannot advance the cursor -- it is counted and dropped. */
static enum cnxman_join_rx join_h_mscp_end(struct cnxman_join *j,
					   const struct join_ev *e)
{
	struct vms_mscp_cl_unit unit;
	int terminator = 0;
	vms_codec_status_t st;

	if (e->conid != j->mscp_conid || e->frame == NULL) {
		j->ignored_events++;
		return CNXMAN_JOIN_RX_CONSUMED;
	}
	j->mscp_ends++;

	st = vms_mscp_cl_fsm_on_scc_end(&j->mscp, e->frame, e->len);
	if (st == VMS_CODEC_OK) {
		if (j->mscp.state == VMS_MSCP_CL_ST_SCC1_DONE)
			join_mscp_send_scc(j);   /* SET CONTROLLER, twice */
		else
			join_mscp_send_gus(j);   /* then the NEXT-UNIT walk */
		return CNXMAN_JOIN_RX_CONSUMED;
	}

	st = vms_mscp_cl_fsm_on_gus_end(&j->mscp, e->frame, e->len, &unit,
					&terminator);
	if (st != VMS_CODEC_OK) {
		j->ignored_events++;
		return CNXMAN_JOIN_RX_CONSUMED;
	}
	if (terminator) {
		join_walk_complete(j);
		return CNXMAN_JOIN_RX_CONSUMED;
	}
	join_record_unit(j, &unit);
	join_mscp_send_gus(j);
	return CNXMAN_JOIN_RX_CONSUMED;
}

/* ==========================================================================
 * Step 5: the burst, the moment the VC is up (spec sec 4(L)(e), sec 4(o))
 * ========================================================================== */

/*
 * The VMS$VAXcluster connection to the member this join runs its dialogue with
 * is OPEN. This is where the joiner stops being a name in a directory and
 * starts being a candidate member, and on the reference wire it happens
 * identically on a connection the joiner opened and on one it accepted (E67).
 */
static void join_cm_advertise(struct cnxman_join *j)
{
	j->cm_open = 1u;
	join_goto(j, CNXMAN_JOIN_ADVERTISE);

	/* sec 4(o) rows 1-2: model, then parameters, on OUR VC. A build that
	 * failed has already put this FSM in FAILED; carrying on from there
	 * would put a second message on a join that has stopped. */
	join_send_model(j);
	if (j->state != (uint8_t)CNXMAN_JOIN_ADVERTISE)
		return;
	join_send_params(j);
	if (j->state != (uint8_t)CNXMAN_JOIN_ADVERTISE)
		return;

	/* ... and only now the disk walk, which is what the real joiner does
	 * in the measured gap before its op-0x02 (sec 4(o)'s own UPDATE). With
	 * no MSCP$DISK on the member there is nothing to walk and admission
	 * starts at once. */
	if (j->mscp_walk_done)
		join_walk_complete(j);
	else
		join_mscp_send_scc(j);
	join_arm_watch(j);
}

static enum cnxman_join_rx join_h_cm_opened(struct cnxman_join *j,
					    const struct join_ev *e)
{
	if (e->conid != j->cm_conid) {
		j->ignored_events++;
		return CNXMAN_JOIN_RX_CONSUMED;
	}
	join_cm_advertise(j);
	return CNXMAN_JOIN_RX_CONSUMED;
}

/*
 * The member won the connect race (E67). Adopt ITS connection as this join's
 * one VMS$VAXcluster connection -- there is exactly one per pair -- and let
 * the drive reach step 4 in its own time; join_open_cm() then finds it already
 * open and advertises on it instead of opening a second. Adopting here rather
 * than advertising here is what keeps step 2's lookup-before-connect and step
 * 3's disk walk in the order the wire measured them.
 *
 * Nothing here is inferred: `peer` and `conid` are the accept path's own
 * facts, and the two cases this join does not own are COUNTED, not acted on:
 *   - another member's connection (the Rule of Total Connectivity requires
 *     accepting it, and it is not this dialogue) -- `cm_other_member`;
 *   - a true simultaneous open, i.e. our own connect is already out --
 *     `cm_already_held`. Adopting then would give the pair TWO connections,
 *     and no capture grounds which side must yield, so this node yields
 *     nothing and invents nothing.
 */
static enum cnxman_join_rx join_h_cm_accepted(struct cnxman_join *j,
					      const struct join_ev *e)
{
	if (!j->target_valid || e->from_sysid != j->target_sysid) {
		j->cm_other_member++;
		return CNXMAN_JOIN_RX_CONSUMED;
	}
	if (j->cm_conid != 0u) {
		j->cm_already_held++;
		return CNXMAN_JOIN_RX_CONSUMED;
	}
	j->cm_conid = e->conid;
	j->cm_open = 1u;
	j->cm_adopted++;
	join_log(j, "%CNXMAN, the cluster opened the VMS$VAXcluster connection "
		    "to this node");
	return CNXMAN_JOIN_RX_CONSUMED;
}

/* ==========================================================================
 * Handlers: the member-driven tail (spec sec 4(o) rows 3, 5-10)
 * ========================================================================== */

/* The peer's own 0x14/0x01/0x02. A real record arriving is the ONLY way a
 * peer's VOTES enters its CSB. */
static enum cnxman_join_rx join_h_peer_advert(struct cnxman_join *j,
					      const struct join_ev *e)
{
	struct vms_csb *csb = join_target_csb(j);
	struct vms_cm_params p;

	j->peer_adverts++;
	if (e->env.opcode != VMS_CM_OP_PARAMS || csb == NULL)
		return CNXMAN_JOIN_RX_CONSUMED;

	if (vms_cm_params_parse(e->frame, e->len, &e->fi, &p) != VMS_CODEC_OK)
		return CNXMAN_JOIN_RX_CONSUMED;

	/*
	 * VOTES is the one parameter this message GROUNDS (spec sec 4(j),
	 * pinned by controlled reconfiguration). EXPECTED_VOTES and QDSKVOTES
	 * have no isolated offset, so they are passed through as the CSB
	 * already holds them rather than being overwritten with a zero this
	 * node could not stand behind.
	 */
	cnxman_csb_set_params(csb, p.votes, csb->expected_votes,
			      csb->qdskvotes);
	return CNXMAN_JOIN_RX_CONSUMED;
}

/* op-0x03 COMMIT and each op-0x05 rebuild transaction: the grounded 0x81 echo,
 * with body[17] carrying THIS node's own current class (spec sec 4(r)). */
static enum cnxman_join_rx join_h_echo(struct cnxman_join *j,
				       const struct join_ev *e)
{
	vms_codec_status_t st;

	if (!join_recipe_allowed(e->env.category, e->env.opcode,
				 (uint16_t)VMS_CM_RECIPE_ECHO)) {
		j->ignored_events++;
		return CNXMAN_JOIN_RX_CONSUMED;
	}
	st = vms_cm_echo_response_build(e->frame, e->len, j->tr_class,
					j->scratch,
					(uint32_t)sizeof(j->scratch), NULL);
	if (join_build_failed(j, st))
		return CNXMAN_JOIN_RX_CONSUMED;
	if (join_emit_cm(j, 1) == 0)
		j->echoes_sent++;
	return CNXMAN_JOIN_RX_CONSUMED;
}

/*
 * The op-0x06 MEMBERSHIP burst -- the anti-LARP crux of this whole item.
 *
 * The allowlist grounds it as CONSUME + "answered only by the opportunistic
 * cat-0x04 ack, never 0x81" (spec sec 4(p)/(u); sec 4(o) row 10 shows the
 * joiner's cat-0x04 answers), so that is what goes back regardless of
 * whether this frame taught us anything.
 *
 * E30 (falsified + replaced by a real-VAX capture,
 * tests/lab/captures/op06-join-20260903.pcap): this frame is the EXISTING
 * coordinator re-asserting a real member's own CSID (its own, or another
 * already-admitted member's -- both share the cluster's one generation).
 * This node reads that CSID (vms_cm_membership_coordinator_csid(), which
 * returns "not found" rather than guessing), takes the WIRE-LEARNED
 * generation off its high 16 bits, and computes ITS OWN CSID from ITS OWN
 * real SCSSYSTEMID (FC-P0.10 SYSGEN state) -- never copied, never
 * templated. cnxman_join_csid_learned() does the rest: fires
 * cnxman_club_learn_local_csid() and moves this node NEW -> MEMBER through
 * the existing [ADMIT|BARRIER][CSID_LEARNED] table cell.
 *
 * Until a shape-valid CSID has been seen in an op-0x06, nothing is learned:
 * the CLUB's local CSID stays unlearned, this node stays NEW, and it issues
 * no DLM traffic. That is the honest outcome; a plausible-looking CSID is
 * the fabrication that bugchecked a real VAX.
 */
static void join_learn_csid_from_membership(struct cnxman_join *j,
					     const struct join_ev *e)
{
	uint32_t coord_csid = 0u;
	uint32_t generation;
	uint32_t own_csid;

	if (j->cl == NULL)
		return;
	if (vms_cm_membership_coordinator_csid(e->frame, e->len, &e->fi,
					       &coord_csid) != VMS_CODEC_OK) {
		j->csid_unpinned++;
		if (j->csid_unpinned == 1u)
			join_log(j, "%CNXMAN, no coordinator CSID seen yet: "
				    "this node's CSID was NOT learned");
		return;
	}
	/* generation = the coordinator CSID's high 16 bits, READ FROM THE
	 * WIRE -- never assumed, never hardcoded (INV-6). */
	generation = (coord_csid >> 16) & 0xffffu;
	own_csid = (generation << 16) |
		   ((uint32_t)j->cl->params.scssystemid & 0x3ffu);
	cnxman_join_csid_learned(j, own_csid);
}

static enum cnxman_join_rx join_h_membership(struct cnxman_join *j,
					     const struct join_ev *e)
{
	vms_codec_status_t st;

	j->membership_records++;
	join_learn_csid_from_membership(j, e);

	st = vms_cm_ack_build(j->scratch, (uint32_t)sizeof(j->scratch), NULL);
	if (join_build_failed(j, st))
		return CNXMAN_JOIN_RX_CONSUMED;
	if (join_emit_cm(j, 1) == 0)
		j->acks_sent++;
	return CNXMAN_JOIN_RX_CONSUMED;
}

/* The cat-0x06 close: answer with THIS node's own parameter block. Echoing the
 * request's payload here bugchecked a real VAX with INCONSTATE (spec sec 4(p)),
 * which is why vms_cm_close_build takes our params and not the request's. */
static enum cnxman_join_rx join_h_close(struct cnxman_join *j,
					const struct join_ev *e)
{
	struct vms_cm_node_params own;
	vms_codec_status_t st;

	if (!join_recipe_allowed(e->env.category, e->env.opcode,
				 (uint16_t)VMS_CM_RECIPE_CLOSE)) {
		j->ignored_events++;
		return CNXMAN_JOIN_RX_CONSUMED;
	}
	join_own_params(j, &own);
	st = vms_cm_close_build(e->frame, e->len, &own, j->scratch,
				(uint32_t)sizeof(j->scratch), NULL);
	if (join_build_failed(j, st))
		return CNXMAN_JOIN_RX_CONSUMED;
	if (join_emit_cm(j, 1) == 0)
		j->closes_answered++;
	return CNXMAN_JOIN_RX_CONSUMED;
}

/* ==========================================================================
 * Handlers: the hand-off to the barrier (FC-P3.5)
 * ========================================================================== */

static enum cnxman_join_rx join_forward(struct cnxman_join *j,
					const struct join_ev *e)
{
	if (j->barrier == NULL)
		return CNXMAN_JOIN_RX_HANDOFF;

	j->handoffs++;
	(void)cnxman_barrier_rx_frame(j->barrier, e->frame, e->len,
				      e->from_csid, e->from_valid);
	return CNXMAN_JOIN_RX_CONSUMED;
}

/*
 * The op-0x0a GO: Phase 2's point of no return (book p. 7-42) and the moment
 * the barrier owns the wire. This FSM originates nothing more of the
 * transition -- the twelve op-0x0b steps, the rebuild echoes and the release
 * handling are all FC-P3.5's, and a join that kept driving here would be two
 * emitters on one connection.
 */
static enum cnxman_join_rx join_h_go(struct cnxman_join *j,
				     const struct join_ev *e)
{
	enum cnxman_join_rx rx = join_forward(j, e);

	if (j->state != (uint8_t)CNXMAN_JOIN_BARRIER &&
	    j->state != (uint8_t)CNXMAN_JOIN_MEMBER) {
		join_goto(j, CNXMAN_JOIN_BARRIER);
		join_log(j, "%CNXMAN, VAXcluster state transition in progress");
	}
	return rx;
}

/* The transition OPEN also carries our own class, which the 0x81 echo of a
 * later request has to report (spec sec 4(r)). Recorded from the real frame,
 * and only when the codec really read it. */
static enum cnxman_join_rx join_h_tr_open(struct cnxman_join *j,
					  const struct join_ev *e)
{
	struct vms_cm_open o;

	if (vms_cm_open_parse(e->frame, e->len, &e->fi, &o) == VMS_CODEC_OK)
		j->tr_class = o.cls;
	return join_forward(j, e);
}

/* ==========================================================================
 * Handlers: loss, the assignment, and the watchdog
 * ========================================================================== */

static enum cnxman_join_rx join_h_closed(struct cnxman_join *j,
					 const struct join_ev *e)
{
	/* Con.ID 0 is never minted (vms_scs_fsm.h SS4: the WIRE uses 0 for
	 * "not bound yet"), so it is a safe "this connection was never
	 * opened" sentinel and a close that names one of ours is recognised
	 * whether or not it ever reached OPEN. */
	if (j->mscp_conid != 0u && e->conid == j->mscp_conid) {
		/* Losing the disk client is never losing the join (E68) --
		 * before the walk or after it. */
		if (!j->mscp_walk_done) {
			j->mscp_lost++;
			join_log(j, "%CNXMAN, lost the MSCP$DISK disk-client "
				    "connection: this node enumerates none of "
				    "that member's units, and the join goes on");
		}
		join_disk_client_gone(j);
		return CNXMAN_JOIN_RX_CONSUMED;
	}
	if (j->cm_conid != 0u && e->conid == j->cm_conid) {
		j->cm_open = 0u;
		join_fail(j, CNXMAN_JOIN_FAIL_PATHLOST,
			  "%CNXMAN, lost a connection needed to join the "
			  "cluster");
		return CNXMAN_JOIN_RX_CONSUMED;
	}
	j->ignored_events++;
	return CNXMAN_JOIN_RX_CONSUMED;
}

static enum cnxman_join_rx join_h_csid_learned(struct cnxman_join *j,
					       const struct join_ev *e)
{
	if (j->cl == NULL)
		return CNXMAN_JOIN_RX_CONSUMED;
	cnxman_club_learn_local_csid(&j->cl->club, e->csid);
	join_goto(j, CNXMAN_JOIN_MEMBER);
	join_log(j, "%CNXMAN, this node is now a VAXcluster member");
	return CNXMAN_JOIN_RX_CONSUMED;
}

/* p. 2-51's own recovery: the poller REPEATS. Nothing expires. */
static enum cnxman_join_rx join_h_watch_lookup(struct cnxman_join *j,
					       const struct join_ev *e)
{
	(void)e;
	j->slow_steps++;
	if (j->slow_steps == 1u)
		join_log(j, "%CNXMAN, waiting for a directory answer from the "
			    "cluster");
	if (join_send_lookups(j) != 0u)
		j->lookups_reissued++;
	join_arm_watch(j);
	return CNXMAN_JOIN_RX_CONSUMED;
}

static enum cnxman_join_rx join_h_watch(struct cnxman_join *j,
					const struct join_ev *e)
{
	(void)e;
	j->slow_steps++;
	if (j->slow_steps == 1u)
		join_log(j, "%CNXMAN, waiting to form or join an OpenVMS "
			    "Cluster");
	join_arm_watch(j);
	return CNXMAN_JOIN_RX_CONSUMED;
}

/*
 * THE WATCHDOG IN [ADVERTISE] AND [ADMIT] -- p. 2-51's "the poller REPEATS",
 * applied to the promotion burst exactly as join_h_watch_lookup() already
 * applies it to the directory round (E70).
 *
 * WHY IT IS NEEDED. The three originations of sec 4(o) are made ONCE, on the
 * instant the drive reaches step 5, and a message SCS REFUSES there is gone:
 * this FSM counted the refusal and then waited for a reply to a message that
 * never left the node, until the connection died under it. That is what
 * happened on the live cluster join-e69 (2026-09-04) -- MODEL, PARAMS and
 * CONFIG were all built, all three were refused by SCS, and the join sat in
 * ADMIT until the circuit was lost. Several of the refusals SCS can make there
 * are TRANSIENT by nature (a spent port send window, a full unacked ring), and
 * the joiner that re-offers on its own beat rides them out; the joiner that
 * originates once cannot.
 *
 * WHAT IT MAY AND MAY NOT RE-OFFER. ONLY a message this node never
 * transmitted. `model_sent`/`params_sent`/`config_sent` are incremented in
 * join_send_*() ONLY when join_emit_cm() returned 0, i.e. only when SCS took
 * the body -- so a zero here is the executive's own record that the message
 * did not go, and a message that DID go is never sent twice. Each re-offer is
 * a fresh origination with its own send-msg# (join_emit_cm advances the CSB's
 * dialogue counter), which is what spec sec 4(j)'s strictly-monotonic-per-
 * sender rule requires; nothing is retransmitted.
 *
 * It re-offers nothing at all while the VMS$VAXcluster connection is not open:
 * with no connection there is no origination to make, and join_emit_cm()'s own
 * gate would refuse it anyway.
 */
static void join_reoffer_burst(struct cnxman_join *j)
{
	uint8_t state = j->state;
	int offered = 0;

	if (!j->cm_open)
		return;
	if (state != (uint8_t)CNXMAN_JOIN_ADVERTISE &&
	    state != (uint8_t)CNXMAN_JOIN_ADMIT)
		return;

	/* IN THE ORDER sec 4(o) MEASURED, and only what is DUE in this state:
	 * rows 1-2 are due from ADVERTISE on, row 6's op-0x02 only once the
	 * disk walk finished -- which is what reaching ADMIT means. So a join
	 * that had its MODEL refused and has since walked the member's disks
	 * still re-offers the MODEL first: an op-0x02 from a node that never
	 * managed to say what it IS advertises nothing. */
	if (j->model_sent == 0u) {
		join_send_model(j);
		offered = 1;
	}
	if (j->state == state && j->params_sent == 0u) {
		join_send_params(j);
		offered = 1;
	}
	if (j->state == state && state == (uint8_t)CNXMAN_JOIN_ADMIT &&
	    j->config_sent == 0u) {
		join_send_config(j);
		offered = 1;
	}

	/* ATTEMPTS, not successes: whether a re-offer got through is what
	 * model_sent/params_sent/config_sent say, and a counter that rose only
	 * on success would report a join stuck against a permanent refusal as
	 * a join that was never re-offered. */
	if (offered)
		j->burst_reoffers++;
}

static enum cnxman_join_rx join_h_watch_burst(struct cnxman_join *j,
					      const struct join_ev *e)
{
	join_reoffer_burst(j);
	return join_h_watch(j, e);
}

/* ==========================================================================
 * The table. [state][event]; NULL = the evidence does not connect that event
 * to that state, so it is ignored and COUNTED rather than guessed.
 * ========================================================================== */

static const join_handler_t
join_table[CNXMAN_JOIN_STATE__COUNT][CNXMAN_EV__COUNT] = {
	/*
	 * [IDLE] only CLUSTER_START starts a join -- but a member may open its
	 * VMS$VAXcluster connection to this node before this node has anything
	 * to join through, and that fact is recorded (as `cm_other_member`:
	 * there is no target to compare it with yet) rather than dropped.
	 */
	[CNXMAN_JOIN_IDLE] = {
		[CNXMAN_EV_START]        = join_h_start,
		[CNXMAN_EV_CM_ACCEPTED]  = join_h_cm_accepted,
	},

	/* [DIR ROUND] spec sec 4(L)(a)+(b): our OWN SCS$DIRECTORY client
	 * round, resolving every name before connecting to it. */
	[CNXMAN_JOIN_DIR_ROUND] = {
		[CNXMAN_EV_DIR_RESULT]   = join_h_dir_result,
		[CNXMAN_EV_CM_ACCEPTED]  = join_h_cm_accepted,
		[CNXMAN_EV_TIMER_JOIN]   = join_h_watch_lookup,
	},

	/* [MSCP CONNECT] spec sec 4(L)(c). */
	[CNXMAN_JOIN_MSCP_CONNECT] = {
		[CNXMAN_EV_CDT_OPEN]     = join_h_mscp_opened,
		[CNXMAN_EV_CM_ACCEPTED]  = join_h_cm_accepted,
		[CNXMAN_EV_CDT_CLOSED]   = join_h_closed,
		[CNXMAN_EV_TIMER_JOIN]   = join_h_watch,
	},

	/* [VC CONNECT] spec sec 4(L)(d). */
	[CNXMAN_JOIN_VC_CONNECT] = {
		[CNXMAN_EV_CDT_OPEN]     = join_h_cm_opened,
		[CNXMAN_EV_CM_ACCEPTED]  = join_h_cm_accepted,
		[CNXMAN_EV_CDT_CLOSED]   = join_h_closed,
		[CNXMAN_EV_TIMER_JOIN]   = join_h_watch,
	},

	/*
	 * [ADVERTISE] the burst is out and the disk walk is running. The peer
	 * reciprocates its own 0x14/0x01 here (sec 4(o) row 3, within ~1 ms),
	 * and a coordinator that opens a transition this early is answered by
	 * the barrier exactly as it would be later.
	 */
	[CNXMAN_JOIN_ADVERTISE] = {
		[CNXMAN_EV_CM_ACCEPTED]  = join_h_cm_accepted,
		[CNXMAN_EV_MSCP_END]     = join_h_mscp_end,
		[CNXMAN_EV_RX_CONFIG]    = join_h_peer_advert,
		[CNXMAN_EV_RX_COMMIT]    = join_h_echo,
		[CNXMAN_EV_RX_MEMBERSHIP] = join_h_membership,
		[CNXMAN_EV_RX_CLOSE]     = join_h_close,
		[CNXMAN_EV_RX_TR_OPEN]   = join_h_tr_open,
		[CNXMAN_EV_RX_TR_GO]     = join_h_go,
		[CNXMAN_EV_RX_REBUILD]   = join_forward,
		[CNXMAN_EV_CDT_CLOSED]   = join_h_closed,
		[CNXMAN_EV_TIMER_JOIN]   = join_h_watch_burst,
	},

	/* [ADMIT] op-0x02 is out; the member drives (sec 4(o) rows 5-10). */
	[CNXMAN_JOIN_ADMIT] = {
		[CNXMAN_EV_CM_ACCEPTED]  = join_h_cm_accepted,
		[CNXMAN_EV_RX_CONFIG]    = join_h_peer_advert,
		[CNXMAN_EV_RX_COMMIT]    = join_h_echo,
		[CNXMAN_EV_RX_MEMBERSHIP] = join_h_membership,
		[CNXMAN_EV_RX_CLOSE]     = join_h_close,
		[CNXMAN_EV_RX_TR_OPEN]   = join_h_tr_open,
		[CNXMAN_EV_RX_TR_GO]     = join_h_go,
		[CNXMAN_EV_RX_REBUILD]   = join_forward,
		[CNXMAN_EV_CSID_LEARNED] = join_h_csid_learned,
		[CNXMAN_EV_CDT_CLOSED]   = join_h_closed,
		[CNXMAN_EV_TIMER_JOIN]   = join_h_watch_burst,
	},

	/*
	 * [BARRIER] FC-P3.5 owns the transition family from here. This FSM
	 * still answers what the barrier does not: the cat-0x06 close (a
	 * recurring member poll, sec 4(p)/(q)) and further membership bursts.
	 */
	[CNXMAN_JOIN_BARRIER] = {
		[CNXMAN_EV_CM_ACCEPTED]  = join_h_cm_accepted,
		[CNXMAN_EV_RX_TR_OPEN]   = join_h_tr_open,
		[CNXMAN_EV_RX_TR_GO]     = join_h_go,
		[CNXMAN_EV_RX_BARRIER]   = join_forward,
		[CNXMAN_EV_RX_BARRIER_ACK] = join_forward,
		[CNXMAN_EV_RX_REBUILD]   = join_forward,
		[CNXMAN_EV_RX_CONFIG]    = join_h_peer_advert,
		[CNXMAN_EV_RX_COMMIT]    = join_h_echo,
		[CNXMAN_EV_RX_MEMBERSHIP] = join_h_membership,
		[CNXMAN_EV_RX_CLOSE]     = join_h_close,
		[CNXMAN_EV_CSID_LEARNED] = join_h_csid_learned,
		[CNXMAN_EV_CDT_CLOSED]   = join_h_closed,
	},

	/* [MEMBER] steady state: the same server obligations, forever. */
	[CNXMAN_JOIN_MEMBER] = {
		[CNXMAN_EV_CM_ACCEPTED]  = join_h_cm_accepted,
		[CNXMAN_EV_RX_TR_OPEN]   = join_h_tr_open,
		[CNXMAN_EV_RX_TR_GO]     = join_h_go,
		[CNXMAN_EV_RX_BARRIER]   = join_forward,
		[CNXMAN_EV_RX_BARRIER_ACK] = join_forward,
		[CNXMAN_EV_RX_REBUILD]   = join_forward,
		[CNXMAN_EV_RX_CONFIG]    = join_h_peer_advert,
		[CNXMAN_EV_RX_COMMIT]    = join_h_echo,
		[CNXMAN_EV_RX_MEMBERSHIP] = join_h_membership,
		[CNXMAN_EV_RX_CLOSE]     = join_h_close,
		[CNXMAN_EV_CDT_CLOSED]   = join_h_closed,
	},

	/* [FAILED] terminal and honest: nothing here revives a join. A new one
	 * is a new CLUSTER_START, which re-inits this FSM. */
	[CNXMAN_JOIN_FAILED] = { 0 },
};

/* ==========================================================================
 * Dispatch
 * ========================================================================== */

/*
 * The ONE fact about an event this ring can carry beyond its name (E69). Which
 * one it is depends on the event, so the mapping is stated here once rather
 * than at each of the eight entry points -- and every value is a field the
 * caller really supplied, never a composed identity.
 *
 * A 48-bit SCSSYSTEMID does not fit a longword, so a peer identity is recorded
 * TRUNCATED to its low 32 bits and the record's own documentation says so: it
 * is a correlation aid for a transcript, and a reader must not treat it as the
 * system id (INV-6 -- half an identity is not an identity).
 */
static uint32_t join_ev_aux(enum cnxman_event ev, const struct join_ev *e)
{
	switch (ev) {
	case CNXMAN_EV_CDT_OPEN:
	case CNXMAN_EV_CDT_CLOSED:
	case CNXMAN_EV_MSCP_END:
		return (uint32_t)e->conid;
	case CNXMAN_EV_CM_ACCEPTED:
		return (uint32_t)e->from_sysid;   /* low 32 bits, see above */
	case CNXMAN_EV_DIR_RESULT:
		return e->present ? 1u : 0u;      /* HIT vs "NOT PRESENT HERE" */
	case CNXMAN_EV_CSID_LEARNED:
		return (uint32_t)e->csid;
	default:
		return 0u;
	}
}

/*
 * EVERY [state][event] pair this FSM evaluates passes through here, so ONE
 * record per dispatch is a complete transcript of the machine -- including the
 * EMPTY CELLS, which are the interesting ones: an event the evidence does not
 * connect to the state it arrived in is counted as `ignored_events` and, from
 * the wire alone, is indistinguishable from an event that never arrived. The
 * record carries the state BEFORE and AFTER the handler, so a transition that
 * fired and a handler that ran and changed nothing read differently.
 */
static enum cnxman_join_rx join_dispatch(struct cnxman_join *j,
					 enum cnxman_event ev,
					 const struct join_ev *e)
{
	join_handler_t h;
	uint8_t before;
	uint32_t aux;
	enum cnxman_join_rx rx;

	if ((unsigned)j->state >= (unsigned)CNXMAN_JOIN_STATE__COUNT)
		return CNXMAN_JOIN_RX_BAD;
	if ((unsigned)ev >= (unsigned)CNXMAN_EV__COUNT)
		return CNXMAN_JOIN_RX_BAD;

	before = j->state;
	aux = join_ev_aux(ev, e);

	h = join_table[j->state][ev];
	if (h == NULL) {
		j->ignored_events++;
		cnxman_diag_dispatch(j->diag, join_now_ms(j), before, j->state,
				     (uint8_t)ev, 0,
				     (uint8_t)CNXMAN_JOIN_RX_CONSUMED, aux);
		return CNXMAN_JOIN_RX_CONSUMED;
	}

	rx = h(j, e);
	cnxman_diag_dispatch(j->diag, join_now_ms(j), before, j->state,
			     (uint8_t)ev, 1, (uint8_t)rx, aux);
	return rx;
}

/* ==========================================================================
 * Classification: which shared event is this frame?
 *
 * Indexed by MEANING, so an opcode re-assignment after a capture is an edit
 * here and nowhere else.
 * ========================================================================== */

/* The transition family the barrier owns (FC-P3.5) -- including the
 * coordinator's 0x81/0x0b step acknowledgement, which is a RESPONSE and so
 * cannot be recognised by opcode alone. */
static int join_is_barrier_frame(const struct vms_cm_envelope *env)
{
	if (vms_wire_is_response(env->category))
		return env->opcode == VMS_CM_OP_BARRIER;
	if (env->category == VMS_CM_CAT_DLM)
		return env->opcode == VMS_CM_OP_DLM_REBUILD;
	if (env->category != VMS_CM_CAT_CONFIG)
		return 0;
	switch (env->opcode) {
	case VMS_CM_OP_XITION_REM:
	case VMS_CM_OP_XITION_ADD:
	case VMS_CM_OP_XITION_GO:
	case VMS_CM_OP_BARRIER:
	case VMS_CM_OP_BARRIER_REL:
	case VMS_CM_OP_DEPART_XITION:
	case VMS_CM_OP_0F:
	case VMS_CM_OP_ABORT:
		return 1;
	default:
		return 0;
	}
}

static enum cnxman_event join_event_of_barrier(const struct vms_cm_envelope *env)
{
	if (vms_wire_is_response(env->category))
		return CNXMAN_EV_RX_BARRIER_ACK;
	if (env->category == VMS_CM_CAT_DLM)
		return CNXMAN_EV_RX_REBUILD;
	switch (env->opcode) {
	case VMS_CM_OP_XITION_GO:
		return CNXMAN_EV_RX_TR_GO;
	case VMS_CM_OP_BARRIER:
	case VMS_CM_OP_BARRIER_REL:
		return CNXMAN_EV_RX_BARRIER;
	default:
		/* the opens (0x08/0x09/0x0d), op-0x0f and the abort all reach
		 * the barrier through the same forwarding cell */
		return CNXMAN_EV_RX_TR_OPEN;
	}
}

static enum cnxman_event join_event_of(const struct vms_cm_envelope *env)
{
	if (join_is_barrier_frame(env))
		return join_event_of_barrier(env);

	if (env->category == VMS_CM_CAT_CONFIG) {
		switch (env->opcode) {
		case VMS_CM_OP_MODEL:
		case VMS_CM_OP_PARAMS:
		case VMS_CM_OP_CONFIG:
			return CNXMAN_EV_RX_CONFIG;
		case VMS_CM_OP_COMMIT:
		case VMS_CM_OP_LOCKRB:
			return CNXMAN_EV_RX_COMMIT;
		case VMS_CM_OP_MEMBERSHIP:
			return CNXMAN_EV_RX_MEMBERSHIP;
		default:
			return CNXMAN_EV__COUNT;
		}
	}
	/*
	 * cat 0x06 op 0x00 is the transaction CLOSE / recurring member poll
	 * (spec sec 4(p)/(q)) -- the join's own steady-state obligation. The
	 * barrier maps CNXMAN_EV_RX_CLOSE onto the cat-0x01 op-0x04 ABORT
	 * instead; the two tables classify their own frames and neither ever
	 * sees the other's, because join_is_barrier_frame() routes the abort
	 * away above.
	 */
	if (env->category == VMS_CM_CAT_MEMBERSHIP &&
	    env->opcode == VMS_CM_OP_CLOSE)
		return CNXMAN_EV_RX_CLOSE;

	return CNXMAN_EV__COUNT;
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

void cnxman_join_init(struct cnxman_join *j, struct vms_cluster *cl,
		      const struct cnxman_ops *ops,
		      const struct cnxman_join_ops *jops)
{
	if (j == NULL)
		return;
	join_bzero(j, (uint32_t)sizeof(*j));
	j->cl = cl;
	j->ops = ops;
	j->jops = jops;
	j->target_csb = -1;
	j->state = (uint8_t)CNXMAN_JOIN_IDLE;
	vms_mscp_cl_fsm_init(&j->mscp);
}

void cnxman_join_set_cfg(struct cnxman_join *j,
			 const struct cnxman_join_cfg *cfg)
{
	if (j == NULL)
		return;
	if (cfg == NULL) {
		join_bzero(&j->cfg, (uint32_t)sizeof(j->cfg));
		return;
	}
	j->cfg = *cfg;
	if (j->cfg.model_len > VMS_CM_MODEL_MAX) {
		/* Refuse the whole declaration rather than truncate a name:
		 * half a model string is a different model. */
		j->cfg.model_len = 0u;
		j->cfg.model_valid = 0u;
	}
}

void cnxman_join_set_barrier(struct cnxman_join *j, struct cnxman_barrier *b)
{
	if (j != NULL)
		j->barrier = b;
}

void cnxman_join_set_diag(struct cnxman_join *j, struct cnxman_diag_ring *r)
{
	if (j != NULL)
		j->diag = r;
}

/* ==========================================================================
 * Events
 * ========================================================================== */

int cnxman_join_start(struct cnxman_join *j)
{
	struct join_ev e;

	if (j == NULL || j->cl == NULL || j->jops == NULL)
		return -1;
	join_bzero(&e, (uint32_t)sizeof(e));
	(void)join_dispatch(j, CNXMAN_EV_START, &e);
	return (j->state == (uint8_t)CNXMAN_JOIN_FAILED) ? -1 : 0;
}

void cnxman_join_opened(struct cnxman_join *j, vms_conid_t conid)
{
	struct join_ev e;

	if (j == NULL)
		return;
	join_bzero(&e, (uint32_t)sizeof(e));
	e.conid = conid;
	(void)join_dispatch(j, CNXMAN_EV_CDT_OPEN, &e);
}

void cnxman_join_closed(struct cnxman_join *j, vms_conid_t conid,
			uint32_t reason)
{
	struct join_ev e;

	if (j == NULL)
		return;
	join_bzero(&e, (uint32_t)sizeof(e));
	e.conid = conid;
	e.reason = reason;
	(void)join_dispatch(j, CNXMAN_EV_CDT_CLOSED, &e);
}

void cnxman_join_rejected(struct cnxman_join *j, vms_conid_t conid,
			  uint32_t reason)
{
	if (j == NULL)
		return;
	/*
	 * The disk-client connect asserted no version identity, so its refusal
	 * is not the p. 2-25 verdict and not a join failure (E68). Counted,
	 * logged, and the drive carries on to the VMS$VAXcluster step.
	 */
	if (j->mscp_conid != 0u && conid == j->mscp_conid) {
		j->mscp_rejected++;
		join_diag_arrival(j, CNXMAN_DIAG_EV_NONE,
				  CNXMAN_DIAG_R_MSCP_REJ, (int32_t)reason,
				  (uint32_t)conid);
		join_log(j, "%CNXMAN, the member refused this node's MSCP$DISK "
			    "disk-client connection: no disk discovery from it, "
			    "and the join goes on");
		join_disk_client_gone(j);
		return;
	}
	/* Only a rejection of a connection THIS join made is this join's
	 * business; anything else belongs to whoever opened it. */
	if (j->cm_conid == 0u || conid != j->cm_conid) {
		j->ignored_events++;
		join_diag_arrival(j, CNXMAN_DIAG_EV_NONE,
				  CNXMAN_DIAG_R_NOT_OURS, (int32_t)reason,
				  (uint32_t)conid);
		return;
	}
	join_diag_arrival(j, CNXMAN_DIAG_EV_NONE, CNXMAN_DIAG_R_CM_REJ,
			  (int32_t)reason, (uint32_t)conid);
	/*
	 * Book p. 2-25 / correction D12: the Connection Managers identify their
	 * version to each other in the 16-byte connect data and REJECT a
	 * version they do not approve of. A rejection is therefore a verdict on
	 * this node's identity, not a transient error, and retrying it is a
	 * loop rather than a recovery.
	 */
	join_fail(j, CNXMAN_JOIN_FAIL_REJECTED,
		  "%CNXMAN, the cluster rejected this node's connection "
		  "(version identity refused)");
}

void cnxman_join_dir_result(struct cnxman_join *j, vms_scs_sysid_t from,
			    const uint8_t *name, int present)
{
	struct join_ev e;

	if (j == NULL || name == NULL)
		return;
	join_bzero(&e, (uint32_t)sizeof(e));
	e.from_sysid = from;
	e.name = name;
	e.present = present;
	(void)join_dispatch(j, CNXMAN_EV_DIR_RESULT, &e);
}

int cnxman_join_holds_disk_client(const struct cnxman_join *j,
				  vms_scs_sysid_t dst)
{
	if (j == NULL || dst == 0u)
		return 0;
	if (!j->target_valid || j->target_sysid != dst)
		return 0;
	return j->mscp_conid != 0u;
}

void cnxman_join_rx_mscp(struct cnxman_join *j, vms_conid_t conid,
			 const uint8_t *frame, uint32_t len)
{
	struct join_ev e;

	if (j == NULL || frame == NULL)
		return;
	join_bzero(&e, (uint32_t)sizeof(e));
	e.conid = conid;
	e.frame = frame;
	e.len = len;
	(void)join_dispatch(j, CNXMAN_EV_MSCP_END, &e);
}

void cnxman_join_cm_accepted(struct cnxman_join *j, vms_scs_sysid_t peer,
			     vms_conid_t conid)
{
	struct join_ev e;

	if (j == NULL)
		return;
	join_bzero(&e, (uint32_t)sizeof(e));
	e.from_sysid = peer;
	e.conid = conid;
	(void)join_dispatch(j, CNXMAN_EV_CM_ACCEPTED, &e);
}

void cnxman_join_csid_learned(struct cnxman_join *j, vms_csid_t csid)
{
	struct join_ev e;

	if (j == NULL)
		return;
	join_bzero(&e, (uint32_t)sizeof(e));
	e.csid = csid;
	(void)join_dispatch(j, CNXMAN_EV_CSID_LEARNED, &e);
}

void cnxman_join_timer(struct cnxman_join *j)
{
	struct join_ev e;

	if (j == NULL)
		return;
	join_bzero(&e, (uint32_t)sizeof(e));
	(void)join_dispatch(j, CNXMAN_EV_TIMER_JOIN, &e);
}

enum cnxman_join_rx cnxman_join_rx_frame(struct cnxman_join *j,
					 const uint8_t *frame, uint32_t len,
					 vms_csid_t from_csid, int from_valid)
{
	struct join_ev e;
	enum cnxman_event ev;
	struct vms_csb *csb;

	if (j == NULL || j->cl == NULL || frame == NULL)
		return CNXMAN_JOIN_RX_BAD;

	join_bzero(&e, (uint32_t)sizeof(e));
	if (vms_frame_classify(frame, len, &e.fi) != VMS_CODEC_OK) {
		/* A body that does not classify never reaches the table, so
		 * without this record it would be indistinguishable from a
		 * frame that never arrived (E69). `aux` carries its length --
		 * the E67 wall was decoded from exactly that. */
		join_diag_arrival(j, CNXMAN_DIAG_EV_NONE,
				  CNXMAN_DIAG_R_UNPARSED, 0, len);
		return CNXMAN_JOIN_RX_BAD;
	}
	if (vms_cm_envelope_parse(frame, len, &e.fi, &e.env) != VMS_CODEC_OK) {
		join_diag_arrival(j, CNXMAN_DIAG_EV_NONE,
				  CNXMAN_DIAG_R_UNPARSED, 0, len);
		return CNXMAN_JOIN_RX_BAD;
	}

	e.frame = frame;
	e.len = len;
	e.from_csid = from_csid;
	e.from_valid = from_valid;

	/* Every body that really arrived from this peer advances the ack side
	 * of the dialogue -- spec sec 4(j): ack-msg# "acknowledges the peer's
	 * highest send-msg# seen". Recorded here, once, from the parsed
	 * envelope, so no emitter ever has to guess it. */
	csb = join_target_csb(j);
	if (csb != NULL)
		cnxman_csb_dialogue_heard(csb, e.env.send_msg);

	/* The opportunistic cat-0x04 ack the member sends is a fact, not a
	 * transition: counted, never answered (spec sec 4(u)). */
	if (e.env.category == VMS_CM_CAT_ACK) {
		j->peer_acks++;
		join_diag_arrival(j, CNXMAN_DIAG_EV_NONE,
				  CNXMAN_DIAG_R_PEER_ACK, 0,
				  join_diag_catop(&e.env));
		return CNXMAN_JOIN_RX_CONSUMED;
	}

	ev = join_event_of(&e.env);
	if (ev == CNXMAN_EV__COUNT) {
		/* A real CM frame no cell of THIS table owns. Recorded with the
		 * (category, opcode) it really carried, because "the join saw a
		 * frame it does not handle" and "nothing arrived" are different
		 * diagnoses and the wire cannot tell them apart. */
		join_diag_arrival(j, CNXMAN_DIAG_EV_NONE,
				  CNXMAN_DIAG_R_NOT_MINE, 0,
				  join_diag_catop(&e.env));
		return CNXMAN_JOIN_RX_NOT_MINE;
	}
	return join_dispatch(j, ev, &e);
}

int cnxman_join_connect_req(struct cnxman_join *j, vms_scs_sysid_t peer,
			    vms_conid_t peer_conid,
			    const uint8_t *conndata, uint32_t conndata_len)
{
	struct vms_csb *csb;

	(void)peer_conid;
	(void)conndata;
	(void)conndata_len;

	if (j == NULL || j->cl == NULL)
		return -1;
	/*
	 * THE RULE OF TOTAL CONNECTIVITY (spec sec 4(y), book p. 7-11): a
	 * joiner is admitted only if EVERY member has a connection to it, so a
	 * member opening its own VMS$VAXcluster connection to us is taken
	 * whatever this join is doing. Refusing one is refusing the join.
	 *
	 * We take it for a system we have a CSB for -- one the port has really
	 * formed a circuit with. A connect from a system with no block is
	 * refused rather than admitted into a table this node cannot describe
	 * (INV-6: there is no "system zero").
	 *
	 * The peer's 16-byte connect data is its version identity (p. 2-25).
	 * It is COUNTED and NOT acted on: every node this project has observed
	 * runs one VMS version, and a version policy built on one data point
	 * would be invented, not implemented (spec sec 4(N)).
	 */
	csb = cnxman_club_find_sysid(&j->cl->club, peer);
	if (csb == NULL) {
		j->inbound_refused++;
		join_diag_arrival(j, CNXMAN_DIAG_EV_NONE,
				  CNXMAN_DIAG_R_REFUSED, 0, (uint32_t)peer);
		join_log(j, "%CNXMAN, refused a VMS$VAXcluster connection from "
			    "a system with no cluster system block");
		return -1;
	}
	j->inbound_accepted++;
	join_diag_arrival(j, CNXMAN_DIAG_EV_NONE, CNXMAN_DIAG_R_ACCEPTED, 0,
			  (uint32_t)peer);
	return 0;
}

/* ==========================================================================
 * Readback
 * ========================================================================== */

int cnxman_join_handed_off(const struct cnxman_join *j)
{
	if (j == NULL)
		return 0;
	return j->state == (uint8_t)CNXMAN_JOIN_BARRIER ||
	       j->state == (uint8_t)CNXMAN_JOIN_MEMBER;
}

uint32_t cnxman_join_units(const struct cnxman_join *j,
			   struct vms_mscp_cl_unit *out, uint32_t cap)
{
	uint32_t i;

	if (j == NULL)
		return 0u;
	if (out != NULL) {
		for (i = 0; i < j->units_found && i < cap; i++)
			out[i] = j->units[i];
	}
	return j->units_found;
}
