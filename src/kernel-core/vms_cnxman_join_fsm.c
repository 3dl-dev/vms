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
	case CNXMAN_JOIN_FAIL_TIMEOUT:   return "reconnect interval expired";
	case CNXMAN_JOIN_FAIL_UNANSWERED:
		return "no member answered the membership request";
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

/*
 * A VERDICT: this attempt is over and repeating it would be a loop, not a
 * recovery. [FAILED] is an empty table row on purpose -- see join_stopped()
 * below for the OTHER outcome, the one three live runs needed.
 */
static void join_fail(struct cnxman_join *j, enum cnxman_join_failure why,
		      const char *msg)
{
	j->failure = (uint8_t)why;
	join_goto(j, CNXMAN_JOIN_FAILED);
	join_log(j, msg);
}

/* ==========================================================================
 * STAYING ALIVE THROUGH A TRANSIENT (E71)
 *
 * THE WALL, three live runs in a row. On each of join-e67refire, join-e69refire
 * and join-e70refire (2026-09-04) this join reached [FAILED] on an EARLY
 * connectivity event, and then -- one to two seconds later -- the member opened
 * its own VMS$VAXcluster connection to this node and OFFERED it the membership
 * dialogue. [FAILED] is an empty row, so each of those offers was counted as an
 * ignored event and dropped, and the node stayed silent for the rest of the
 * run. The E70 transcript is the whole story in five records: MSCP_CONNECT,
 * cdt-closed rc=3, MSCP_CONNECT->FAILED at t+14.473, and at t+16.399 a genuine
 * cm-accept landing in the empty [FAILED] row.
 *
 * WHY THAT WAS WRONG, from the book rather than from the symptom:
 *
 *   1. CONNECTIVITY IS NOT A VERDICT. p. 7-30: when a connection to a remote
 *      connection manager goes for any reason not involving a last gasp, "for a
 *      limited period of time, the local Connection Manager will attempt once a
 *      second to establish another connection", and "do not presume that the
 *      remote system has left ... simply because the local Connection Manager
 *      has lost contact". A machine that ends the whole join on the first lost
 *      or refused connection presumes exactly that.
 *
 *   2. THE RECONNECT APPARATUS ALREADY EXISTS, AND IT IS NOT HERE. p. 7-23:
 *      "Module CNXMAN in the local Connection Manager is responsible for
 *      managing SCS connections between the local SYS$CLUSTER and remote
 *      SYS$CLUSTERs. It is also responsible for performing reconnect attempts
 *      if any of those SCS connections are lost", and the state of each such
 *      connection is a field of the CSB -- the ten connectivity states of
 *      pp. 7-23/7-24, which vms_cnxman_csb.c implements: WAIT ("a timeout is in
 *      progress; at the end of the timeout, an attempt will be made to
 *      reconnect ... repeated until either connectivity is once again
 *      established ... or a time limit is exceeded"), RECONNECT, and REACCEPT
 *      ("the local Connection Manager is accepting a reconnect request from the
 *      remote Connection Manager"). So the retry policy and its bound are the
 *      CSB's, this FSM's job is to still be there when connectivity returns,
 *      and there is exactly ONE reconnect policy in this executive.
 *
 *   3. REACCEPT IS THE ANSWER THE LIVE RUNS WERE GIVEN. In both transcripts the
 *      member re-offered the connection itself. A joiner that is alive takes it
 *      (join_h_cm_accepted); a joiner in [FAILED] cannot.
 *
 * WHAT THIS DOES NOT DO (INV-6). It never retries into a membership: a retry
 * re-establishes a CONNECTION and nothing else. No CSID is learned, no CSB flag
 * is set and no cluster state is written anywhere in this file's retry path --
 * the only route to MEMBER remains a real op-0x06 naming a real generation
 * (join_learn_csid_from_membership). "Keep trying until it looks joined" would
 * be the fabrication; "keep trying until the member really answers, or the
 * executive says the window is over" is the machine.
 * ========================================================================== */

/*
 * This attempt stopped for a CONNECTIVITY reason. The named reason is kept --
 * an operator has to be able to read why the last attempt did not get anywhere
 * -- and the FSM goes back to IDLE, where the caller's own once-a-second sweep
 * may start a genuinely NEW join (a fresh target selection, a fresh directory
 * round, a fresh connect). Nothing about this node's membership is asserted on
 * the way through: it was not a member before and it is not one now.
 */
static void join_stopped(struct cnxman_join *j, enum cnxman_join_failure why,
			 const char *msg)
{
	j->failure = (uint8_t)why;
	j->target_valid = 0u;
	j->target_csb = -1;
	j->lookups_hit = 0u;
	j->lookups_answered = 0u;
	j->mscp_conid = 0u;
	j->mscp_open = 0u;
	j->mscp_walk_done = 0u;
	j->cm_conid = 0u;
	j->cm_open = 0u;
	j->burst_on_conn = 0u;
	/* The admission clock belongs to a request this attempt made on a
	 * connection it held; both are gone (E80). */
	j->admit_answered = 0u;
	j->admit_target_acked = 0u;
	j->admit_silent_beats = 0u;
	j->units_found = 0u;
	vms_mscp_cl_fsm_init(&j->mscp);
	join_goto(j, CNXMAN_JOIN_IDLE);
	join_log(j, msg);
}

/*
 * Take `conid` as THE VMS$VAXcluster connection of this join.
 *
 * The burst mask is cleared here and nowhere else: a different connection is a
 * different dialogue, so whatever this node managed to say down the last one
 * has not been said down this one.
 */
static void join_cm_take(struct cnxman_join *j, vms_conid_t conid)
{
	if (j->cm_conid != conid)
		j->burst_on_conn = 0u;
	j->cm_conid = conid;
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
	struct vms_csb *c;

	if (j->cl == NULL || !j->target_valid || j->target_csb < 0)
		return NULL;
	c = cnxman_club_csb_at(&j->cl->club, (uint32_t)j->target_csb);
	if (c == NULL)
		return NULL;
	/*
	 * The slot must still hold the SYSTEM this join selected. A CSB is
	 * deallocated and rebuilt when a system returns (p. 7-25), so a slot
	 * index alone can come back pointing at somebody else -- and every
	 * caller of this uses the answer to stamp an envelope or to read a
	 * connection identity. Answering NONE is the honest outcome (INV-6);
	 * answering "the block that is in that slot now" is how a message gets
	 * addressed with another system's dialogue counters.
	 */
	if (!c->sysid_valid || c->sysid != j->target_sysid)
		return NULL;
	return c;
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
	/*
	 * E77: the envelope is stamped from `csb`'s dialogue state but the body
	 * goes out on the Con.ID THIS JOIN holds, and the two are only the same
	 * conversation while the CSB's dialogue is bound to that connection. In
	 * production they are (the glue binds the accepted/connected Con.ID into
	 * the CSB at the instant SCS mints it, before this join adopts it), so
	 * this gate is the assertion that they still are -- and a REFUSAL, not a
	 * stamp, when they are not: a send-msg#/ack-msg# pair carried onto a
	 * connection it does not describe is the byte a real VAX's Connection
	 * Manager bugchecks on (CNXMGRERR, E76/E77).
	 */
	if (!cnxman_csb_dialogue_is_on(csb, (uint32_t)j->cm_conid))
		return CNXMAN_DIAG_G_SKEW;
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
	cnxman_envelope_originate(csb, j->scratch, is_response);

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

/*
 * A codec refusal and a send refusal are different facts and are counted as
 * such. There is no body when the codec refuses, so the ring records the
 * refusal with both wire bytes explicitly zero -- an omission, not a message.
 * Returns nonzero when there is nothing to send.
 */
static int join_note_codec_failure(struct cnxman_join *j, vms_codec_status_t st)
{
	if (st == VMS_CODEC_OK)
		return 0;
	j->codec_failures++;
	join_diag_emit(j, 0u, 0u, CNXMAN_DIAG_G_CODEC, (int32_t)st,
		       j->cm_conid);
	return 1;
}

/* The same, for a body on the JOIN'S OWN drive: a codec this node cannot get a
 * message out of is a defect in this node, and retrying it is a loop rather
 * than a recovery -- so the attempt ends honestly.
 *
 * The per-peer identity beat deliberately does NOT use this (E73): that beat
 * runs on every CSB whether or not a join is in flight, including on a node
 * that is already a MEMBER, and a build refusal there is a reason to say
 * nothing to that peer -- never a reason to unmake a membership. */
static int join_build_failed(struct cnxman_join *j, vms_codec_status_t st)
{
	if (!join_note_codec_failure(j, st))
		return 0;
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

/*
 * WHAT THIS NODE HAS ALREADY SAID ABOUT ITSELF ON THE CONNECTION IT HOLDS TO
 * `csb` RIGHT NOW (E73). Keyed to the Con.ID, so a connection that changed
 * makes the record stale by construction and the identity goes out again --
 * the same per-connection rule `burst_on_conn` applies to the join's own
 * dialogue, applied to every peer.
 */
/*
 * `conid` is the connection the record would go out ON, and the two callers
 * name a DIFFERENT one on purpose: the join's own dialogue rides the Con.ID
 * the join holds (`j->cm_conid`, which it may have adopted from the member's
 * own connect before the CSB was updated), while the per-peer beat rides the
 * one the CSB records (`csb->cdt_conid`). In production those are the same
 * value for the member a join is driving through -- the glue writes the
 * accepted/connected Con.ID into the CSB at the instant SCS mints it -- so the
 * two paths share one mask and neither can send the same record twice. A
 * Con.ID of 0 is "no connection", never "connection zero" (INV-6).
 */
static int join_advert_due(const struct vms_csb *csb, vms_conid_t conid,
			   uint8_t bit)
{
	if (csb == NULL || conid == 0u)
		return 0;
	if (csb->cm_advert_conid != (uint32_t)conid)
		return 1;   /* a different connection: nothing was said on it */
	return (csb->cm_advert_sent & bit) == 0u;
}

static void join_advert_mark(struct vms_csb *csb, vms_conid_t conid,
			     uint8_t bit)
{
	if (csb == NULL || conid == 0u)
		return;
	if (csb->cm_advert_conid != (uint32_t)conid) {
		csb->cm_advert_conid = (uint32_t)conid;
		csb->cm_advert_sent = 0u;
	}
	csb->cm_advert_sent |= bit;
}

/* Build this node's model advertisement into `scratch`. Nonzero when the codec
 * refused, which has already ended the join honestly. */
static vms_codec_status_t join_build_model(struct cnxman_join *j)
{
	const uint8_t *name = j->cfg.model_valid ? j->cfg.model : NULL;
	uint8_t len = j->cfg.model_valid ? j->cfg.model_len : 0u;

	if (!j->cfg.model_valid)
		j->model_omitted++;
	return vms_cm_model_build(name, len, j->scratch,
				  (uint32_t)sizeof(j->scratch), NULL);
}

static void join_send_model(struct cnxman_join *j)
{
	struct vms_csb *csb = join_target_csb(j);

	if (!join_advert_due(csb, j->cm_conid, CNXMAN_JOIN_B_MODEL))
		return;   /* this connection has already carried it */
	if (join_build_failed(j, join_build_model(j)))
		return;
	if (join_emit_cm(j, 0) == 0) {
		j->model_sent++;
		j->burst_on_conn |= CNXMAN_JOIN_B_MODEL;
		join_advert_mark(csb, j->cm_conid, CNXMAN_JOIN_B_MODEL);
	}
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

/* Build this node's cluster-parameters record into `scratch`, VOTES read from
 * the REAL SYSGEN parameters this node booted with -- never a default (spec
 * sec 4(j) pinned body[22:24] byte-exact across four configured values; 0 is a
 * legitimate one). */
static vms_codec_status_t join_build_params(struct cnxman_join *j)
{
	struct vms_cm_node_params own;
	uint16_t votes = 0u;

	if (j->cl != NULL)
		votes = j->cl->params.votes;

	join_own_params(j, &own);
	join_note_lockdirwt(j);
	return vms_cm_params_build(votes, &own, j->scratch,
				   (uint32_t)sizeof(j->scratch), NULL);
}

static void join_send_params(struct cnxman_join *j)
{
	struct vms_csb *csb = join_target_csb(j);

	if (!join_advert_due(csb, j->cm_conid, CNXMAN_JOIN_B_PARAMS))
		return;
	if (join_build_failed(j, join_build_params(j)))
		return;
	if (join_emit_cm(j, 0) == 0) {
		j->params_sent++;
		j->burst_on_conn |= CNXMAN_JOIN_B_PARAMS;
		join_advert_mark(csb, j->cm_conid, CNXMAN_JOIN_B_PARAMS);
	}
}

/* ==========================================================================
 * THE IDENTITY EXCHANGE IS PER-PEER, NOT PER-JOIN (E73 part A)
 *
 * THE WALL, live (join-e72refire, 2026-09-04). This node's promotion burst
 * reached the wire for the first time -- and it reached exactly ONE member.
 * VAX2 had won the connect race, so the join drove VAX2; VAX1's own
 * VMS$VAXcluster connection had arrived while the join was still in [IDLE],
 * where no cell handles it, so VAX1 never heard this node say what it is. Its
 * CSB for OVMXJ1 stayed at `State: 09 wait, votes 0/0` for the whole run, and
 * VAX1 is the node CLUSTER_NODES was read from.
 *
 * WHAT THE REFERENCE DOES, decoded frame by frame from
 * vax3-2to3-established-join-20260730 (the joiner is 08:00:2b:11:22:33):
 *
 *   t+29.8253  J -> VAX1   cat 0x01 op 0x14 (snd 1), op 0x01 (snd 2)
 *   t+29.8256  VAX1 -> J   the same two back, snd 1 and 2
 *   t+30.3692  VAX2 -> J   op 0x14 (snd 1)   [VAX2 opened this VC]
 *   t+30.3692  J -> VAX2   op 0x14 (snd 1), op 0x01 (snd 2)
 *   t+30.3694  VAX2 -> J   op 0x01 (snd 2)
 *   t+34.7634  J -> VAX2   cat 0x01 op 0x02  -- to the COORDINATOR ONLY
 *
 * So the pair (MODEL, PARAMS) goes to EVERY member, on that member's own VC,
 * each with its own send-msg# starting at 1 -- and the members send theirs
 * back the same way, i.e. it is what a connection manager says on every
 * VMS$VAXcluster connection it has, in both directions, whether it is joining
 * or already a member. Only op-0x02 is the join's single act, and it goes to
 * one peer (spec sec 4(o): "A non-coordinator peer SILENTLY DISCARDS op 0x02").
 *
 * SO THIS IS A PER-CSB OBLIGATION AND IT RUNS ON THE BEAT, not out of a join
 * state. It asserts nothing about membership: it says this node's own model
 * string and its own SYSGEN VOTES, addressed by the CSB whose connection the
 * executive really holds, stamped from that CSB's own dialogue counters. A
 * peer whose connection is not OPEN is skipped, not queued -- p. 7-24's OPEN
 * is the executive's own record of connectivity and this reads it.
 * ========================================================================== */

/*
 * Originate one already-built body to `csb` (CLUB slot `idx`).
 *
 * ADDRESSED BY CSB, like the barrier's steps and for the same reason: a peer's
 * CSID is not something this node can learn (integration notes E30, E73), and
 * the connection the executive holds for that system is. The dialogue counter
 * is advanced BEFORE the stamp, which is what makes the first message to a
 * fresh peer carry send-msg# 1 (spec sec 4(j), "Starts at 1 on the first VC
 * message"; cnxman_csb_dialogue_sent()'s own note).
 */
static int join_emit_to_csb(struct cnxman_join *j, struct vms_csb *csb,
			    int32_t idx)
{
	uint8_t cat, op;
	int rc;

	join_body_kind(j, &cat, &op);
	if (j->ops == NULL || j->ops->send_csb == NULL) {
		j->send_failures++;
		join_diag_emit(j, cat, op, CNXMAN_DIAG_G_NO_OPS, 0,
			       csb->cdt_conid);
		return -1;
	}

	cnxman_envelope_originate(csb, j->scratch, 0);

	rc = j->ops->send_csb(j->ops->ctx, idx, j->scratch, VMS_CM_BODY_LEN);
	if (rc != 0) {
		j->send_failures++;
		join_diag_emit(j, cat, op, CNXMAN_DIAG_G_REFUSED, (int32_t)rc,
			       csb->cdt_conid);
		return -1;
	}
	join_diag_emit(j, cat, op, CNXMAN_DIAG_G_SENT, 0, csb->cdt_conid);
	return 0;
}

/* Is this CSB a peer whose VMS$VAXcluster connection the executive says is
 * OPEN? Both halves are the CSB ladder's own state (p. 7-23/7-24), written
 * from a real CDT open and from nothing else. */
static int join_peer_advertisable(const struct vms_csb *csb,
				  const struct vms_csb *local)
{
	return csb != NULL && csb != local && csb->in_use && csb->sysid_valid &&
	       csb->cdt_conid != 0u &&
	       csb->state == (uint8_t)VMS_CNXMAN_CSB_OPEN;
}

/* One peer: whichever of the two identity records this connection has not
 * carried yet. Each is built fresh and stamped from THIS peer's counters --
 * nothing is copied from another peer's frame (INV-6). */
static void join_advert_peer(struct cnxman_join *j, struct vms_csb *csb,
			     int32_t idx)
{
	if (join_advert_due(csb, csb->cdt_conid, CNXMAN_JOIN_B_MODEL)) {
		if (join_note_codec_failure(j, join_build_model(j)))
			return;
		if (join_emit_to_csb(j, csb, idx) == 0) {
			j->model_sent++;
			j->peer_adverts_sent++;
			join_advert_mark(csb, csb->cdt_conid,
					 CNXMAN_JOIN_B_MODEL);
		}
	}
	if (join_advert_due(csb, csb->cdt_conid, CNXMAN_JOIN_B_PARAMS)) {
		if (join_note_codec_failure(j, join_build_params(j)))
			return;
		if (join_emit_to_csb(j, csb, idx) == 0) {
			j->params_sent++;
			j->peer_adverts_sent++;
			join_advert_mark(csb, csb->cdt_conid,
					 CNXMAN_JOIN_B_PARAMS);
		}
	}
}

void cnxman_join_advertise_peers(struct cnxman_join *j)
{
	struct vms_club *club;
	struct vms_csb *local;
	uint32_t i;

	if (j == NULL || j->cl == NULL)
		return;
	club = &j->cl->club;
	local = cnxman_club_local(club);

	for (i = 0; i < club->n_csb; i++) {
		struct vms_csb *c = cnxman_club_csb_at(club, i);

		if (!join_peer_advertisable(c, local))
			continue;
		if (!join_advert_due(c, c->cdt_conid, CNXMAN_JOIN_B_MODEL) &&
		    !join_advert_due(c, c->cdt_conid, CNXMAN_JOIN_B_PARAMS))
			continue;
		j->peers_advertised++;
		join_advert_peer(j, c, (int32_t)i);
	}
}

static void join_send_config(struct cnxman_join *j)
{
	vms_codec_status_t st;

	st = vms_cm_config_build(j->scratch, (uint32_t)sizeof(j->scratch),
				 NULL);
	if (join_build_failed(j, st))
		return;
	if (join_emit_cm(j, 0) == 0) {
		j->config_sent++;
		j->burst_on_conn |= CNXMAN_JOIN_B_CONFIG;
	}
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
	/*
	 * Message events. `body` is the SYSAP's OWN bytes and nothing below
	 * them -- 132 for a VMS$VAXcluster body, the MSCP end class's own
	 * length on the disk-client connection. Design sec 3.2.4: SCS "calls
	 * scs_sysap_ops.message(ctx, local_conid, frame + 72, inner_len - 16)".
	 * There is no struct vms_frame_info here because a SYSAP never sees a
	 * frame to classify (E73).
	 */
	const uint8_t         *body;
	uint32_t               len;
	struct vms_cm_envelope env;
	vms_csid_t             from_csid;
	int                    from_valid;
	/*
	 * WHICH CSB this message arrived on -- the CLUB slot of the system at
	 * the other end of the connection SCS delivered it through, or -1 when
	 * the glue could not resolve one. Carried alongside `from_csid` because
	 * the two are different facts: a CSID is an identity the CLUSTER
	 * assigns and this node has no way to learn for a PEER, while the CSB
	 * is the executive's own record of the connection the message came in
	 * on (book p. 7-23). The barrier addresses the coordinator by this
	 * (E73) -- see cnxman_barrier_rx_body().
	 */
	int32_t                from_csb;

	/* connection events */
	vms_conid_t conid;
	uint32_t    reason;

	/* directory result */
	const uint8_t  *name;
	vms_scs_sysid_t from_sysid;
	int             present;

	/* the assignment the cluster made */
	vms_csid_t csid;

	/* CNXMAN_EV_TRANSITION_DONE: the barrier's commit record, read back --
	 * the class off the wire, and what the coordinator's nodemap said
	 * about this node (three-valued; see cnxman_phase2_stats). */
	uint8_t aux_class;
	uint8_t aux_named;
	uint8_t aux_in_map;
};

typedef enum cnxman_join_rx (*join_handler_t)(struct cnxman_join *,
					      const struct join_ev *);

/* Defined with the table below; used by join_post_commit(), which posts the
 * commit as a second top-level dispatch once the barrier hand-off has
 * returned. */
static enum cnxman_join_rx join_dispatch(struct cnxman_join *j,
					 enum cnxman_event ev,
					 const struct join_ev *e);

/* ==========================================================================
 * Starting: pick the member, declare our directory descriptor, run our own
 * SCS$DIRECTORY client round
 * ========================================================================== */

/*
 * Has the executive GIVEN UP on the connection to this system?
 *
 * Both answers are the CSB's own connectivity state, written by the ladder in
 * vms_cnxman_csb.c and by nothing else: DISCONNECT is where csb_give_up() parks
 * a connection whose p. 7-30 reconnect window ran out (or that was closed in an
 * orderly way), and DEAD is p. 7-24's "the CSB whose connection state is DEAD
 * represents the old incarnation". Neither is a connection manager this node
 * can join THROUGH, and neither is a guess made here.
 */
static int join_csb_abandoned(const struct vms_csb *c)
{
	return c->state == (uint8_t)VMS_CNXMAN_CSB_DISCONNECT ||
	       c->state == (uint8_t)VMS_CNXMAN_CSB_DEAD;
}

/*
 * Does the executive have CONNECTIVITY to this member? p. 7-24 OPEN: "An SCS
 * connection exists (i.e., the local Connection Manager has connectivity to the
 * remote Connection Manager) ... This is the normal state of a CSB." The CSB
 * ladder writes that state from a real CDT open (vms_cnxman_csb.c h_open) and
 * from nothing else, so this is a READ of the executive's own answer, never
 * this FSM's opinion of it.
 */
static int join_csb_connected(const struct vms_csb *c)
{
	return c->state == (uint8_t)VMS_CNXMAN_CSB_OPEN;
}

/* ==========================================================================
 * THE DECLINED SET (E80)
 *
 * One bit per CLUB slot: the members THIS attempt has already asked for
 * admission and got silence from. It excludes them from re-selection, which is
 * what makes "ask the next member" terminate instead of cycling, and it is
 * cleared at the start of every fresh attempt (join_h_start) because a member
 * that could not coordinate an admission six seconds ago is not refused
 * forever. It asserts nothing about any member -- see the header for the
 * keyed-by-slot consequence, which is stated rather than hidden.
 * ========================================================================== */

static int join_slot_declined(const struct cnxman_join *j, uint32_t slot)
{
	if (slot >= (uint32_t)VMS_CLUB_MAX_CSB)
		return 0;
	return (j->declined[slot >> 5] & (1u << (slot & 31u))) != 0u;
}

static void join_slot_decline(struct cnxman_join *j, uint32_t slot)
{
	if (slot >= (uint32_t)VMS_CLUB_MAX_CSB)
		return;
	j->declined[slot >> 5] |= (1u << (slot & 31u));
}

static void join_declined_clear(struct cnxman_join *j)
{
	uint32_t i;

	for (i = 0; i < (uint32_t)CNXMAN_JOIN_DECLINE_WORDS; i++)
		j->declined[i] = 0u;
}

/*
 * Is the CSB in slot `slot` a member this ATTEMPT may still ask?
 *
 * Every clause is a read of executive state: the CLUB's own local block, the
 * CSB's `sysid_valid`, the ladder's connectivity state (p. 7-24 DISCONNECT and
 * DEAD are connections it has given up on), and this attempt's own record of
 * whom it has already asked.
 */
static int join_askable(const struct cnxman_join *j, const struct vms_csb *c,
			const struct vms_csb *local, uint32_t slot)
{
	return c != NULL && c != local && c->sysid_valid &&
	       !join_csb_abandoned(c) && !join_slot_declined(j, slot);
}

/* Adopt slot `slot` as the member this join drives through. The two unusable
 * ranking rules are COUNTED here (see below), once per selection. */
static void join_set_target(struct cnxman_join *j, const struct vms_csb *c,
			    uint32_t slot)
{
	j->target_sysid = c->sysid;
	j->target_csb = (int32_t)slot;
	j->target_valid = 1u;
	j->target_level_unpinned++;
	j->member_count_ungated++;
}

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

		if (!join_askable(j, c, local, i - 1u))
			continue;
		join_set_target(j, c, i - 1u);
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

/*
 * A start that could not happen. NOT a failed join: nothing was asserted to
 * anybody and no connection was made, so there is nothing to fail -- this node
 * simply has no cluster to join AT THIS INSTANT, which is precisely the state
 * VMS reports as "waiting to form or join an OpenVMS Cluster" and keeps
 * REPEATING (E71; the same p. 2-51 "the poller REPEATS" that already governs
 * the directory round). The reason is named, the deferral is counted, and the
 * FSM stays in IDLE so the caller's next beat asks again.
 */
static enum cnxman_join_rx join_start_deferred(struct cnxman_join *j,
					       enum cnxman_join_failure why,
					       const char *msg)
{
	j->failure = (uint8_t)why;
	j->starts_deferred++;
	if (j->starts_deferred == 1u)
		join_log(j, msg);
	join_goto(j, CNXMAN_JOIN_IDLE);
	return CNXMAN_JOIN_RX_CONSUMED;
}

/*
 * IS A BACK-OFF STILL RUNNING? (E80)
 *
 * Set when every member declined this node's membership request, and sized by
 * the executive's own RECNXINTERVAL -- p. 7-30's reconnect interval, the one
 * number VMS already uses for "how long before asking the cluster again",
 * loaded from SYSGEN into the CLUB (cnxman_club_init). A CLUB that carries no
 * interval contributes nothing and the next beat asks again: this FSM does not
 * invent a delay it cannot ground.
 *
 * The comparison is wrap-safe, and `retry_at_valid` 0 is "no back-off is owed"
 * rather than "at time zero" (INV-6).
 */
static int join_backoff_pending(struct cnxman_join *j)
{
	if (!j->retry_at_valid)
		return 0;
	if ((int32_t)(join_now_ms(j) - j->retry_at_ms) < 0)
		return 1;
	j->retry_at_valid = 0u;   /* it elapsed: this is the fresh attempt */
	return 0;
}

static void join_backoff_start(struct cnxman_join *j)
{
	uint32_t secs = 0u;

	if (j->cl != NULL)
		secs = (uint32_t)j->cl->club.recnxinterval;
	if (secs == 0u) {
		j->retry_at_valid = 0u;
		return;
	}
	j->retry_at_ms = join_now_ms(j) + (secs * 1000u);
	j->retry_at_valid = 1u;
}

static enum cnxman_join_rx join_h_start(struct cnxman_join *j,
					const struct join_ev *e)
{
	(void)e;

	if (join_backoff_pending(j)) {
		j->starts_backed_off++;
		return join_start_deferred(j, CNXMAN_JOIN_FAIL_UNANSWERED,
					   "%CNXMAN, waiting out the reconnect "
					   "interval before asking the cluster "
					   "for admission again");
	}
	/* A FRESH ATTEMPT ASKS EVERYBODY. The declined set is per-attempt (E80):
	 * a member that did not coordinate the last admission is not refused,
	 * and the next attempt puts it back in the pool. */
	join_declined_clear(j);

	if (join_select_target(j) != 0)
		return join_start_deferred(j, CNXMAN_JOIN_FAIL_NO_TARGET,
					   "%CNXMAN, waiting to form or join an "
					   "OpenVMS Cluster");
	join_declare_dir_data(j);

	if (join_send_lookups(j) == 0u)
		return join_start_deferred(j, CNXMAN_JOIN_FAIL_CONNECT,
					   "%CNXMAN, could not open an "
					   "SCS$DIRECTORY connection to the "
					   "cluster: retrying");
	j->failure = (uint8_t)CNXMAN_JOIN_FAIL_NONE;
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
 * Our VMS$VAXcluster connect could not be put on the wire (E71). This is NOT
 * the p. 2-25 version gate -- no connect data reached a peer, no peer judged
 * anything, and a node the member is about to dial cannot be said to have been
 * refused by the cluster. What it is, is "no connectivity yet", which is
 * VC_CONNECT: the beat retries it and the member's own connect is still taken.
 */
static void join_cm_connect_refused(struct cnxman_join *j)
{
	join_cm_take(j, 0u);      /* honest: this node holds no such connection */
	j->cm_open = 0u;
	j->cm_connect_refused++;
	/* NAMED, though not terminal: this is why the attempt is where it is,
	 * and CNXTRACE's summary line is how an operator reads that. It is
	 * cleared the moment connectivity is really achieved. */
	j->failure = (uint8_t)CNXMAN_JOIN_FAIL_CONNECT;
	if (j->cm_connect_refused == 1u)
		join_log(j, "%CNXMAN, this node could not put its VMS$VAXcluster "
			    "connect on the wire: retrying, and still accepting "
			    "the member's own");
	join_goto(j, CNXMAN_JOIN_VC_CONNECT);
	join_arm_watch(j);
}

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
	vms_conid_t conid = 0u;

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
		      CNXMAN_JOIN_CM_CREDITS, &conid) != 0) {
		join_cm_connect_refused(j);
		return;
	}
	join_cm_take(j, conid);
	join_goto(j, CNXMAN_JOIN_VC_CONNECT);
	join_arm_watch(j);
}

/* Everything that becomes true when this node holds no disk-client connection:
 * it claims none, and there is no walk left for anything to wait for. */
static void join_disk_client_clear(struct cnxman_join *j)
{
	j->mscp_open = 0u;
	j->mscp_conid = 0u;
	j->mscp_walk_done = 1u;
}

/*
 * Step 3: the disk-client connection, reachable only from a real HIT.
 *
 * A refusal HERE is this node's own SCS declining to open it, and it is
 * governed by the same rule as the member's REJECT and as a lost disk-client
 * connection: MSCP$DISK IS NOT A MEMBERSHIP PREREQUISITE (E68, below). All
 * three leave the join in the same position -- no served units to enumerate --
 * so all three are counted, said out loud, and stepped over.
 */
static void join_open_mscp(struct cnxman_join *j)
{
	if (join_open(j, cnxman_join_name_disk_cl_drvr,
		      cnxman_join_name_mscp_disk, NULL,
		      CNXMAN_JOIN_MSCP_CREDITS, &j->mscp_conid) != 0) {
		j->mscp_connect_refused++;
		join_disk_client_clear(j);
		join_log(j, "%CNXMAN, this node could not open an MSCP$DISK "
			    "disk-client connection: it enumerates none of that "
			    "member's units, and the join goes on");
		join_open_cm(j);
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
	join_disk_client_clear(j);

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

	if (e->conid != j->mscp_conid || e->body == NULL) {
		j->ignored_events++;
		return CNXMAN_JOIN_RX_CONSUMED;
	}
	j->mscp_ends++;

	st = vms_mscp_cl_fsm_on_scc_end(&j->mscp, e->body, e->len);
	if (st == VMS_CODEC_OK) {
		if (j->mscp.state == VMS_MSCP_CL_ST_SCC1_DONE)
			join_mscp_send_scc(j);   /* SET CONTROLLER, twice */
		else
			join_mscp_send_gus(j);   /* then the NEXT-UNIT walk */
		return CNXMAN_JOIN_RX_CONSUMED;
	}

	st = vms_mscp_cl_fsm_on_gus_end(&j->mscp, e->body, e->len, &unit,
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
	/* Connectivity was achieved: whatever the last attempt stopped for is
	 * no longer this join's position (E71). */
	j->failure = (uint8_t)CNXMAN_JOIN_FAIL_NONE;
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
 * 3's disk walk in the order the wire measured them: the connection's own
 * CDT_OPEN (join_h_cm_opened, which the glue raises immediately after this
 * one) is what advances the drive, and it does so only from the state that is
 * waiting for connectivity.
 *
 * Nothing here is inferred: `peer` and `conid` are the accept path's own
 * facts, and the two cases this join does not own are COUNTED, not acted on --
 * see the two guards below.
 */
static enum cnxman_join_rx join_h_cm_accepted(struct cnxman_join *j,
					      const struct join_ev *e)
{
	/*
	 * Another member's connection. The Rule of Total Connectivity requires
	 * this node to ACCEPT it (cnxman_join_connect_req does), and it is NOT
	 * the dialogue this single-target join runs -- the op-0x02 that starts
	 * admission goes to one coordinator (spec sec 4(o)).
	 *
	 * WHAT THIS NODE STILL OWES IT is its IDENTITY, and since E73 that is
	 * not this handler's job: cnxman_join_advertise_peers() sends the
	 * op-0x14/op-0x01 pair to every member whose connection the CSB ladder
	 * calls OPEN, on the connection manager's own beat, exactly as the
	 * reference joiner did with VAX1 and VAX2 alike. So this cell records
	 * the fact and changes nothing, which is now the whole truth rather
	 * than a gap.
	 *
	 * SAID OUT LOUD, ONCE. On a live cluster this counter and `cm_adopted`
	 * are the difference between "the offer this join was waiting for" and
	 * "an offer from a member it is not driving through", and the two look
	 * identical in a transition ring that can only carry one longword of
	 * `aux`. An operator reading a join that is not progressing has to be
	 * able to tell them apart.
	 */
	if (!j->target_valid || e->from_sysid != j->target_sysid) {
		j->cm_other_member++;
		if (j->cm_other_member == 1u)
			join_log(j, "%CNXMAN, a cluster member opened a "
				    "VMS$VAXcluster connection to this node that "
				    "is not the member this join is driving "
				    "through");
		return CNXMAN_JOIN_RX_CONSUMED;
	}

	/*
	 * A TRUE SIMULTANEOUS OPEN: this join already holds a connection to
	 * this member that is really OPEN. Taking a second would give the pair
	 * two, and no capture grounds which side must yield -- so this node
	 * yields nothing and invents nothing.
	 */
	if (j->cm_conid != 0u && j->cm_open) {
		j->cm_already_held++;
		return CNXMAN_JOIN_RX_CONSUMED;
	}

	/*
	 * A Con.ID THIS JOIN HOLDS IS NOT A CONNECTION THIS NODE HAS (E72).
	 *
	 * THE WALL, live (join-e71refire, 2026-09-04). This join was holding
	 * the Con.ID of a VMS$VAXcluster connect that never reached OPEN when
	 * the member opened ITS OWN connection to this node and SCS brought it
	 * up. The old test here was `cm_conid != 0`, so the genuine, OPEN
	 * membership connection was counted as a simultaneous open and dropped;
	 * the CDT_OPEN that followed it named a Con.ID this join was not
	 * holding and was ignored; and the join sat in [VC CONNECT] for the
	 * remaining 1471 beats of the run with ZERO cat-0x01 frames on the wire.
	 *
	 * THE EXECUTIVE'S OWN LADDER ALREADY DECIDES THIS CASE THE OTHER WAY.
	 * p. 7-24 REACCEPT is "the local Connection Manager is accepting a
	 * reconnect request from the remote Connection Manager", and
	 * vms_cnxman_csb.c's table takes CNXMAN_CSB_EV_CONNECT_RCVD in WAIT and
	 * in RECONNECT -- i.e. WHILE THIS NODE'S OWN ATTEMPT IS IN FLIGHT --
	 * straight to h_reaccept, whose own comment is "the peer beat our
	 * once-a-second attempt to it". A join that refuses what its own CSB
	 * ladder is accepting contradicts the connection manager it is part of.
	 *
	 * So: the connection this node HAS beats the connect it merely ISSUED.
	 * That is a read of two facts the executive supplies -- `cm_open`, set
	 * only from a real CDT open or a real accept, and this event, which the
	 * glue raises only when SCS has the accepted connection OPEN -- and not
	 * a rule about who yields. The superseded connect is COUNTED and left
	 * alone: this file does not tear down a connection SCS may still be
	 * completing, and if it does complete, its CDT_OPEN names a Con.ID this
	 * join no longer holds and is ignored and counted like any other.
	 */
	if (j->cm_conid != 0u)
		j->cm_superseded++;

	join_cm_take(j, e->conid);
	j->cm_open = 1u;
	j->cm_adopted++;
	join_log(j, "%CNXMAN, the cluster opened the VMS$VAXcluster connection "
		    "to this node");
	return CNXMAN_JOIN_RX_CONSUMED;
}

/* ==========================================================================
 * Handlers: the member-driven tail (spec sec 4(o) rows 3, 5-10)
 * ========================================================================== */

/*
 * The peer's own 0x14/0x01/0x02. A real record arriving is the ONLY way a
 * peer's VOTES enters its CSB.
 *
 * INTO THE CSB IT ARRIVED ON, not into the join's target (E73). This used to
 * write `join_target_csb()`, which is right only when the sender IS the member
 * this join is driving through -- and since this node now advertises its own
 * identity to EVERY member on the beat, every member reciprocates, so a
 * target-only write would either drop a member's real VOTES or file them under
 * the wrong system. The arriving CSB is the executive's own answer to "who is
 * at the other end of this connection" (book p. 7-23).
 */
static enum cnxman_join_rx join_h_peer_advert(struct cnxman_join *j,
					      const struct join_ev *e)
{
	struct vms_csb *csb;
	struct vms_cm_params p;

	j->peer_adverts++;
	if (e->env.opcode != VMS_CM_OP_PARAMS || e->from_csb < 0)
		return CNXMAN_JOIN_RX_CONSUMED;
	csb = cnxman_club_csb_at(&j->cl->club, (uint32_t)e->from_csb);
	if (csb == NULL || !csb->in_use)
		return CNXMAN_JOIN_RX_CONSUMED;

	if (vms_cm_params_parse(e->body, e->len, &p) != VMS_CODEC_OK)
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
	st = vms_cm_echo_response_build(e->body, e->len, j->tr_class,
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
 * ==========================================================================
 * E79: THIS HANDLER ANSWERS NOTHING, AND THAT IS THE FIX.
 *
 * It used to build a cat-0x04 and emit it, ONE PER op-0x06, on the reading
 * that sec 4(o) row 10 makes the cat-0x04 this burst's answer. Against the
 * live cluster that put 254 cat-0x04 frames on the wire in 31.6 ms (~8000/s)
 * and VAX2 took a fatal INVEXCEPTN, "Exception while above ASTDEL or on
 * interrupt stack". OVMX halted a real VAX.
 *
 * The reference refutes the 1:1 reading outright, and names the real law.
 * Measured over vax3-2to3-established-join-20260730.pcap, the joiner's own
 * link to the coordinator:
 *
 *     254 op-0x06 in  ->  84 cat-0x04 out.   Ratio 3.02.
 *
 * with the ack-msg word those carriers stamp advancing by exactly 3 across 83
 * of 83 in-burst gaps -- never 1, never 2 -- and the credit field at abs 62
 * reading 3 on 85 of the 86 carriers on that link. Corroborated corpus-wide at
 * 6549 acks from six responder nodes, ack-word advance >= 3 in every one.
 *
 * So the cat-0x04 is a CREDIT CARRIER on a COALESCING QUANTUM of three
 * released buffers (SCS_CREDIT_COALESCE), which is what sec 4(u) was
 * describing when it said the ack is "prompt, opportunistic, cumulative, and
 * NEVER KEYED TO AN OPCODE". It is emitted by cnxman_credit_carrier()
 * (vms_cnxman.c) out of the CDT's real ledger; the burst is CONSUMED here.
 * Three released buffers ARE three consumed peer messages, so a carrier
 * emitted at the quantum stamps credit 3 and advances the ack word by 3
 * without either number being chosen anywhere.
 *
 * The three opcodes sec 4(o) row 10 lists -- 0x04/0x49, 0x04/0x00, 0x04/0x02
 * -- are not three messages. They are ONE message drawn from a rotating pool
 * of three recycled buffers (they recur 28/34/28 times across this burst), and
 * sec 4(p) grounds body[9] as uninitialised buffer content: the 0x49 frame
 * reads "\x04\x49IR_LOOKUP  SCS$DIRECTORY" -- a buffer that last held
 * "DIR_LOOKUP  SCS$DIRECTORY" with body[8] overwritten by the category -- and
 * the 0x02 frame is that node's own earlier op-0x02, same one byte
 * overwritten. vms_cm_ack_build() sends zeros, per sec 4(p)'s "do not
 * reproduce another implementation's uninitialised memory".
 * ==========================================================================
 *
 * E30 (falsified + replaced by a real-VAX capture,
 * tests/lab/captures/op06-join-20260903.pcap): this frame is the EXISTING
 * coordinator re-asserting a real member's own CSID (its own, or another
 * already-admitted member's -- both share the cluster's one generation).
 * This node reads that CSID (vms_cm_membership_coordinator_csid(), which
 * returns "not found" rather than guessing), takes the WIRE-LEARNED
 * generation off its high 16 bits, and computes ITS OWN CSID from ITS OWN
 * real SCSSYSTEMID (FC-P0.10 SYSGEN state) -- never copied, never
 * templated. cnxman_join_csid_learned() records it in the CLUB.
 *
 * >> AND THAT IS ALL IT DOES (E79). << Learning a CSID is not being admitted.
 * The same live run that flooded VAX2 also reached the join FSM's MEMBER state
 * off this event, before a single barrier frame had been exchanged, because
 * the [ADMIT|BARRIER][CSID_LEARNED] cell used to promote. sec 4(q) is explicit
 * that membership "FOLLOWS FROM THE TRANSITION COMPLETING", and measures the
 * joiner's CSID on the wire ~160 ms BEFORE the barrier even opens -- so a CSID
 * seen in an op-0x06 is evidence of a generation, never of admission.
 * join_h_transition_done() owns the promotion now, on the real op-0x0c #12.
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
	if (vms_cm_membership_coordinator_csid(e->body, e->len,
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
	j->membership_records++;
	join_learn_csid_from_membership(j, e);
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
	st = vms_cm_close_build(e->body, e->len, &own, j->scratch,
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
	(void)cnxman_barrier_rx_body(j->barrier, e->body, e->len,
				     e->from_csid, e->from_valid, e->from_csb);
	return CNXMAN_JOIN_RX_CONSUMED;
}

/*
 * HOW THIS FSM HEARS THAT THE TRANSITION COMMITTED (E79).
 *
 * By READING the barrier's commit counter around a dispatch, not by being
 * called back. A callback would give the barrier a pointer to whoever wants
 * promoting and a second reason to fire, and the design keeps the barrier
 * ignorant of its audience (vms_cnxman_barrier_fsm.h: "it does not join").
 *
 * The post is a SECOND TOP-LEVEL DISPATCH, not a nested one: the handler that
 * forwarded to the barrier has already returned, so the transcript reads as two
 * ordered records (RX_BARRIER, then TRANSITION_DONE) and the table is never
 * re-entered from inside itself.
 */
static uint32_t join_barrier_commits(const struct cnxman_join *j)
{
	return cnxman_barrier_commits(j->barrier, NULL);
}

static void join_post_commit(struct cnxman_join *j, uint32_t before,
			     const struct join_ev *src)
{
	struct cnxman_barrier_commit c;
	struct join_ev e;

	join_bzero(&c, (uint32_t)sizeof(c));
	if (cnxman_barrier_commits(j->barrier, &c) == before)
		return;   /* nothing committed in that dispatch */

	join_bzero(&e, (uint32_t)sizeof(e));
	e.from_csid   = src->from_csid;
	e.from_valid  = src->from_valid;
	e.from_csb    = src->from_csb;
	e.aux_class   = c.tr_class;
	e.aux_named   = c.local_named;
	e.aux_in_map  = c.local_in_map;
	(void)join_dispatch(j, CNXMAN_EV_TRANSITION_DONE, &e);
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

	if (vms_cm_open_parse(e->body, e->len, &o) == VMS_CODEC_OK)
		j->tr_class = o.cls;
	return join_forward(j, e);
}

/* ==========================================================================
 * Handlers: loss, the assignment, and the watchdog
 * ========================================================================== */

/*
 * The VMS$VAXcluster connection this join was using is gone (E71).
 *
 * TWO different positions, and the book separates them:
 *
 *   - ALREADY A MEMBER (BARRIER, MEMBER). p. 7-30: "do not presume that the
 *     remote system has left, or will be leaving the cluster simply because
 *     the local Connection Manager has lost contact", and membership is HELD
 *     across the break while the CSB's reconnect window runs. So this FSM does
 *     NOT move: unmaking this node's membership because one connection blinked
 *     is exactly the presumption the book forbids, and what happens next --
 *     reconnect, or a state transition when the window expires -- belongs to
 *     the CSB ladder and the coordinator, which the glue already drives on the
 *     same close.
 *
 *   - NOT YET ADMITTED. There is no membership to hold and what the join needs
 *     is its connection back, which is VC_CONNECT.
 *
 * Either way the Con.ID is dropped. It has to be: the member's REACCEPT
 * (p. 7-24) arrives as an ACCEPTED connection with a NEW Con.ID, and
 * join_h_cm_accepted() refuses to adopt one while this join still claims to
 * hold another. Keeping a dead Con.ID here made the live runs drop the member's
 * own re-offer as `cm_already_held`.
 */
static void join_cm_connection_gone(struct cnxman_join *j)
{
	uint8_t state = j->state;

	j->cm_open = 0u;
	join_cm_take(j, 0u);
	j->cm_lost++;
	j->failure = (uint8_t)CNXMAN_JOIN_FAIL_PATHLOST;   /* named, not fatal */

	if (state == (uint8_t)CNXMAN_JOIN_BARRIER ||
	    state == (uint8_t)CNXMAN_JOIN_MEMBER) {
		join_log(j, "%CNXMAN, lost the VMS$VAXcluster connection to a "
			    "cluster member");
		return;
	}
	join_log(j, "%CNXMAN, lost the VMS$VAXcluster connection before this "
		    "node was admitted: waiting for it to come back");
	join_goto(j, CNXMAN_JOIN_VC_CONNECT);
	join_arm_watch(j);
}

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
		join_cm_connection_gone(j);
		return CNXMAN_JOIN_RX_CONSUMED;
	}
	j->ignored_events++;
	return CNXMAN_JOIN_RX_CONSUMED;
}

/*
 * The CSID this node computed from a real op-0x06 is RECORDED, and that is the
 * whole of it (E79). It used to promote to MEMBER here, which put this node in
 * its member state on the strength of a membership record -- sec 4(q): "There
 * is no 'you are now a member' message ... membership FOLLOWS FROM THE
 * TRANSITION COMPLETING", and the reference joiner's CSID is on the wire
 * ~160 ms BEFORE its barrier even opens. Promoting here asserted a membership
 * the cluster had not granted, which is exactly the fabrication INV-6 forbids.
 */
static enum cnxman_join_rx join_h_csid_learned(struct cnxman_join *j,
					       const struct join_ev *e)
{
	if (j->cl == NULL)
		return CNXMAN_JOIN_RX_CONSUMED;
	cnxman_club_learn_local_csid(&j->cl->club, e->csid);
	return CNXMAN_JOIN_RX_CONSUMED;
}

/*
 * THE PROMOTION, AND THE TWO REAL FACTS IT NEEDS (E79).
 *
 * Reached only from CNXMAN_EV_TRANSITION_DONE, which join_forward() posts when
 * the barrier's commit counter has moved -- one genuine op-0x0c #12 from the
 * coordinator, counted in barrier_finish() and nowhere else.
 *
 * That alone is not enough, because a member also barriers through OTHER
 * nodes' transitions. The second fact is this node's own CSB carrying the
 * member flag, which cnxman_phase2_commit() sets from the nodemap bit the
 * coordinator really asserted for us (p. 7-42 task 1/3). Both are executive
 * reads. If the coordinator ran a transition that did not name this node, the
 * flag is not set and this node does not promote -- which is the honest
 * outcome and the one a `SHOW CLUSTER` reading `NEW` correctly describes.
 */
static enum cnxman_join_rx join_h_transition_done(struct cnxman_join *j,
						  const struct join_ev *e)
{
	if (j->cl == NULL)
		return CNXMAN_JOIN_RX_CONSUMED;

	/*
	 * NO CSID, NO MEMBERSHIP -- however far the transition got (E73, and
	 * the assertion that item left behind). A member IS its CSID: it is
	 * what the CLUB indexes it by, what the DLM would name it by, and what
	 * every later transition's nodemap addresses. A node that answered all
	 * twelve barrier steps and learned no CSID has met the cluster's
	 * obligations and still holds no identity in it, and reading MEMBER
	 * there would be a membership with nothing behind it. So the barrier
	 * still runs -- refusing it breaks the cluster (sec 4(p)) -- and this
	 * node still does not promote.
	 */
	if (!j->cl->club.local_csid_valid) {
		j->commits_not_ours++;
		return CNXMAN_JOIN_RX_CONSUMED;
	}

	/*
	 * THE ONE THING THAT BLOCKS A PROMOTION: the coordinator's nodemap
	 * SAID something about this node, and what it said was no. Kept
	 * separate from "it said nothing" -- sec 4(p) records that the bitmap
	 * byte "holds only 8 slots while the library already reaches slot 5"
	 * and that its true width is UNDETERMINED, so a slot this executive
	 * cannot express is silence, and treating silence as a refusal would
	 * make a real admission unreachable for exactly the clusters we have
	 * the least evidence about. Silence promotes on the completion alone,
	 * which is sec 4(q)'s rule, and is COUNTED so the transcript says the
	 * corroboration was missing rather than implying it was present.
	 */
	if (e->aux_named && !e->aux_in_map) {
		j->commits_not_ours++;
		return CNXMAN_JOIN_RX_CONSUMED;
	}
	if (!e->aux_named)
		j->commits_unmapped++;

	if (j->state == (uint8_t)CNXMAN_JOIN_MEMBER)
		return CNXMAN_JOIN_RX_CONSUMED;

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

/* ==========================================================================
 * THE WATCHDOG IN [VC CONNECT] -- p. 7-30's "attempt once a second", with the
 * BOUND read off the CSB rather than kept here (E71)
 *
 * This beat asks the executive three questions in the order that makes each
 * answer meaningful, and acts on the first that answers:
 *
 *   1. Does the executive hold a VMS$VAXcluster connection to this member, and
 *      is it OPEN? Both answers are the CSB's (p. 7-23/7-24) and neither is
 *      kept here -- join_cm_sync_with_csb() below reads them, takes a Con.ID
 *      this join was not holding, and advertises on a connection the ladder
 *      says is OPEN. E72: the OPEN half is asked EVERY beat, not only on a
 *      beat that changed the Con.ID, because a join already holding the right
 *      Con.ID when the connection came up would otherwise never advertise at
 *      all -- 1471 silent beats on the live cluster.
 *
 *   2. Has the executive GIVEN UP on that member? p. 7-24 WAIT: the reconnect
 *      is "repeated until either connectivity is once again established with
 *      the remote Connection Manager, or a time limit is exceeded, as described
 *      in Section 7.10". The ladder owns that limit (max(RECNXINTERVAL, the
 *      remote-supplied number), p. 7-30) and parks the CSB in DISCONNECT when
 *      it expires. That is the HONEST end of the wait, and it is the executive
 *      that decides it -- this FSM keeps no deadline of its own, so there is
 *      exactly one reconnect policy in the connection manager.
 *
 *   3. Otherwise: if this node holds no connect at all, make one. If it has one
 *      outstanding, wait -- p. 7-30's cadence is one attempt a second, not one
 *      per beat per state.
 * ========================================================================== */

/*
 * The reconnect window ran out. Nothing is asserted, nothing is retried behind
 * the operator's back: the attempt is released with its reason named, and this
 * node goes back to waiting for a cluster -- exactly as truthfully as it waited
 * before it ever saw one.
 */
static void join_no_connectivity(struct cnxman_join *j)
{
	j->connect_windows_expired++;
	join_stopped(j, CNXMAN_JOIN_FAIL_TIMEOUT,
		     "%CNXMAN, the reconnect interval expired with no "
		     "VMS$VAXcluster connection to the member: this node is NOT "
		     "a cluster member");
}

/*
 * RECONCILE THIS JOIN WITH THE CSB, ONCE A BEAT (E71, extended by E72).
 *
 * Two facts live on the CSB and neither of them lives here: WHICH Con.ID the
 * executive holds for this member, and WHETHER that connection is OPEN. This
 * reads both and acts on them, in that order.
 *
 * THE CON.ID. p. 7-23 makes the CSB the record of "the state of the SCS
 * connection between the local SYS$CLUSTER and the SYS$CLUSTER residing in the
 * system associated with the CSB", and the glue writes that connection's Con.ID
 * into `cdt_conid` at the instant SCS mints it -- for this join's own connect,
 * for a reconnect the ladder issued, and for one this node accepted. A
 * `cdt_conid` this join is not holding IS the executive telling it about a
 * connection. Taking it is a read; minting one would not be.
 *
 * THE OPEN (E72 -- the wall this closes). Advertising used to happen ONLY on
 * the beat that CHANGED the Con.ID, so a join that was already holding the
 * right Con.ID when the connection came up -- because the CDT_OPEN for it
 * arrived while this join was holding a different one, or was consumed by
 * another state -- never made its step-5 originations at all. That is the
 * live join-e71refire transcript exactly: [VC CONNECT], 1471 watchdog beats,
 * not one cat-0x01 frame. So the OPEN is now acted on whether or not the
 * Con.ID moved, and it is the CSB's OPEN -- the executive's own record of
 * connectivity -- that fires it.
 *
 * WHY ADVERTISING HERE IS ORDERED CORRECTLY. This runs only in [VC CONNECT],
 * which is reachable only through join_open_cm(), which is reachable only from
 * join_lookups_complete() (both names ANSWERED by the member's own directory)
 * and from the two MSCP$DISK outcomes downstream of it. Step 2's
 * lookup-before-connect and step 3's disk-client connect have therefore both
 * already happened, and join_cm_advertise() itself either resumes the walk or,
 * when there is nothing to walk, releases op-0x02 -- so nothing is skipped and
 * nothing is sent early.
 */
static void join_cm_sync_with_csb(struct cnxman_join *j, struct vms_csb *csb)
{
	if (csb->cdt_conid == 0u)
		return;   /* the executive holds no connection to this member */

	if (csb->cdt_conid != j->cm_conid) {
		join_cm_take(j, csb->cdt_conid);
		j->cm_resynced++;
		join_log(j, "%CNXMAN, adopting the VMS$VAXcluster connection the "
			    "executive holds for this member");
	}
	if (join_csb_connected(csb))
		join_cm_advertise(j);
}

static void join_vc_beat(struct cnxman_join *j)
{
	struct vms_csb *csb = join_target_csb(j);

	if (csb == NULL) {
		join_no_connectivity(j);
		return;
	}

	join_cm_sync_with_csb(j, csb);
	if (j->state != (uint8_t)CNXMAN_JOIN_VC_CONNECT)
		return;   /* connectivity was there: the drive has moved on */

	if (join_csb_abandoned(csb)) {
		join_no_connectivity(j);
		return;
	}
	if (j->cm_conid != 0u)
		return;   /* an attempt -- ours, or the ladder's -- is outstanding */

	j->cm_reattempts++;
	join_open_cm(j);
}

static enum cnxman_join_rx join_h_watch_vc(struct cnxman_join *j,
					   const struct join_ev *e)
{
	join_vc_beat(j);
	if (j->state != (uint8_t)CNXMAN_JOIN_VC_CONNECT)
		return CNXMAN_JOIN_RX_CONSUMED;   /* the beat moved the drive */
	return join_h_watch(j, e);
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
 * WHAT IT MAY AND MAY NOT RE-OFFER. ONLY a message this node never transmitted
 * ON THE CONNECTION IT HOLDS NOW. `burst_on_conn` is set in join_send_*() ONLY
 * when join_emit_cm() returned 0, i.e. only when SCS took the body, and it is
 * cleared the instant the Con.ID changes (join_cm_take) -- so a clear bit is
 * the executive's own record that THIS connection has not carried that message,
 * and a message that DID go down it is never sent twice. (E71: the lifetime
 * `*_sent` counters cannot answer that question. After a reconnect they are
 * nonzero for a dialogue the new connection never had, and reading them here
 * silently stopped the re-offer for good.) Each re-offer is a fresh origination
 * with its own send-msg# (join_emit_cm advances the CSB's dialogue counter),
 * which is what spec sec 4(j)'s strictly-monotonic-per-sender rule requires;
 * nothing is retransmitted.
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
	if ((j->burst_on_conn & CNXMAN_JOIN_B_MODEL) == 0u) {
		join_send_model(j);
		offered = 1;
	}
	if (j->state == state &&
	    (j->burst_on_conn & CNXMAN_JOIN_B_PARAMS) == 0u) {
		join_send_params(j);
		offered = 1;
	}
	if (j->state == state && state == (uint8_t)CNXMAN_JOIN_ADMIT &&
	    (j->burst_on_conn & CNXMAN_JOIN_B_CONFIG) == 0u) {
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

/* ==========================================================================
 * THE MEMBER THIS NODE ASKED IS NOT ANSWERING -- ASK THE NEXT ONE (E80)
 *
 * THE WALL, live (join-e79refire, 2026-09-04). Everything below admission was
 * fixed and clean: the connection, the identity exchange, the credit ledger in
 * both directions, the op-0x06 flood, the barrier. This node put its whole
 * sec 4(o) burst on the wire and reached ADMIT -- and sat there for the rest of
 * the run at CLUSTER_NODES = 2. It had sent its op-0x02 to VAX1. VAX1's console
 * recorded "received VAXcluster membership request from node OVMXJ1" and VAX1
 * never proposed anything. The runs that DID reach a commit (E74-reset, E78)
 * were the runs where the connect race happened to make this node drive VAX2.
 * Same build, same cluster: a coin toss decided whether the join progressed.
 *
 * WHY THE COIN TOSS EXISTS, AND WHY THE FIX IS NOT "PICK THE RIGHT MEMBER".
 * There is no coordinator to identify. The member that RECEIVES op-0x02
 * BECOMES the coordinator of this admission: it runs the quorum arithmetic and
 * asks for the coordinator lock (book p. 7-2 -- which coordinator a cluster
 * gets is "effectively random" -- and pp. 7-37/7-38's JOIN CLUSTER, decided in
 * E74's ruling). Every member is a LEGAL choice; what this node cannot know in
 * advance is whether the one it picked will take the job right now. So the
 * answer is not a better ranking -- it is asking the next member when the one
 * it asked says nothing, which is what a real VAX does (E74's control: a real
 * VAX accepted a second membership request 6.2 s after the first).
 *
 * WHAT IS AND IS NOT AN ANSWER. A willing coordinator PROPOSES within a few
 * milliseconds (E74-reset measured 0.6 ms from this node's op-0x02 to VAX2's
 * op-0x03 COMMIT). So the fact that stops the clock is that member's own
 * op-0x03 -- or any of the transition traffic that follows from it -- arriving
 * on that member's own CSB. Its identity records, its cat-0x04 credit carriers
 * and its recurring member poll are NOT answers: a member sends those whether
 * or not it is coordinating anything, and E73's VAX1 acknowledged this node's
 * traffic all run without ever proposing. That distinction is kept in the
 * transcript rather than folded away (`declines_after_ack`).
 *
 * AND IT IS ONE MEMBER AT A TIME. The reference joiner sends exactly ONE
 * op-0x02 per attempt; a non-coordinator peer silently discards one (sec 4(o)),
 * and fanning the request out would invite two members to propose two
 * transitions for the same node. This re-issues to ONE next member, excludes
 * the ones already asked, and when they have all been asked it stops, backs off
 * one RECNXINTERVAL and starts a genuinely new attempt.
 *
 * NOTHING HERE ASSERTS MEMBERSHIP (INV-6). A re-issue re-asks a question. No
 * CSID is learned, no CSB flag is set, no count is committed, and the only
 * route to MEMBER remains the coordinator's real op-0x0c
 * (join_h_transition_done).
 *
 * AND IT ADDS NO CRASH VECTOR (the standing rule: OVMX never crashes a peer).
 * Read against tools/cluster/cm_wire_safety_audit.py's classes:
 *   S1/S2 (the E76 CNXMGRERR envelope) -- the re-issued bodies are stamped from
 *     the NEW target CSB's own dialogue, which the executive bound to that
 *     Con.ID (E77), so they open at send-msg# 1 or continue that connection's
 *     own count, and they ack only what that peer has really sent on it. The
 *     declined member's counters are never carried across.
 *   S3/S4 (the E78 INVEXCEPTN flood) -- a re-issue emits NO cat-0x04 and no
 *     response of any kind; it is three originations at most, once per member
 *     per attempt, bounded below by six seconds and above by the number of
 *     members, then a RECNXINTERVAL back-off.
 *   S8/S9/S10 -- every body is an ORIGINATION carrying that CSB's real txn and
 *     token on a real, nonzero Con.ID; join_emit_gate() refuses rather than
 *     stamping when any of that is missing.
 *   S12 (credit over-send) -- nothing here bypasses SCS: a send with no credit
 *     is refused and counted, exactly as on the first attempt.
 * ========================================================================== */

/*
 * Compose "%CNXMAN, membership request to <node> not answered" into the
 * context's own line buffer, with the node name taken from the destination
 * CSB's LEARNED `scsnode` -- real executive state, never a formatted
 * placeholder. With no name learned there is nothing to name, and the line says
 * "the selected member" instead of printing an empty one (INV-6).
 */
static uint32_t join_msg_put(struct cnxman_join *j, uint32_t at,
			     const char *text)
{
	uint32_t i = 0;

	while (text[i] != '\0' && (at + i) < (CNXMAN_JOIN_MSGBUF - 1u)) {
		j->msgbuf[at + i] = text[i];
		i++;
	}
	j->msgbuf[at + i] = '\0';
	return at + i;
}

static uint32_t join_msg_put_node(struct cnxman_join *j, uint32_t at,
				  const struct vms_csb *csb)
{
	uint32_t i;

	for (i = 0; i < (uint32_t)csb->scsnode_len &&
		    (at + i) < (CNXMAN_JOIN_MSGBUF - 1u); i++)
		j->msgbuf[at + i] = (char)csb->scsnode[i];
	j->msgbuf[at + i] = '\0';
	return at + i;
}

static void join_log_not_answered(struct cnxman_join *j,
				  const struct vms_csb *csb)
{
	uint32_t n;

	n = join_msg_put(j, 0u, "%CNXMAN, membership request to ");
	if (csb != NULL && csb->scsnode_len > 0u)
		n = join_msg_put_node(j, n, csb);
	else
		n = join_msg_put(j, n, "the selected member");
	(void)join_msg_put(j, n, " not answered");
	join_log(j, j->msgbuf);
}

/*
 * IS A MEMBERSHIP REQUEST REALLY OUTSTANDING RIGHT NOW?
 *
 * Three reads, and the clock runs only while all three hold: this node holds an
 * open VMS$VAXcluster connection, the op-0x02 really went out ON THAT
 * connection (`burst_on_conn` is set only when SCS took the body, and cleared
 * the instant the Con.ID changes), and no answer has arrived. A request SCS
 * refused was never asked and is not timed -- join_reoffer_burst() owns that
 * case, and timing it would decline a member that never heard the question.
 */
static int join_admit_request_outstanding(const struct cnxman_join *j)
{
	return j->cm_open != 0u &&
	       (j->burst_on_conn & CNXMAN_JOIN_B_CONFIG) != 0u &&
	       !j->admit_answered;
}

/*
 * The next member to ask: the CSB nearest the CLUB queue tail that this attempt
 * has not already asked AND that the executive holds an OPEN VMS$VAXcluster
 * connection to (p. 7-24 OPEN, plus a real Con.ID to put the body on).
 *
 * The OPEN requirement is not a shortcut, it is the honest bound on what can be
 * asked NOW: a member this node has no connection to cannot be sent an op-0x02
 * at all, and building one is the drive's job, from the start of an attempt,
 * with its lookups in the order the wire measured them. So a re-issue reaches
 * the members that are reachable this instant, and the members that are not are
 * reached by the next attempt.
 *
 * Returns the CLUB slot, or -1.
 */
static int32_t join_next_askable(struct cnxman_join *j, struct vms_csb **out)
{
	struct vms_club *club;
	struct vms_csb *local;
	uint32_t i;

	if (j->cl == NULL)
		return -1;
	club = &j->cl->club;
	local = cnxman_club_local(club);

	for (i = club->n_csb; i > 0u; i--) {
		struct vms_csb *c = cnxman_club_csb_at(club, i - 1u);

		if (!join_askable(j, c, local, i - 1u))
			continue;
		if (!join_csb_connected(c) || c->cdt_conid == 0u)
			continue;
		*out = c;
		return (int32_t)(i - 1u);
	}
	return -1;
}

/*
 * Every member has been asked. Release the attempt with its reason NAMED, back
 * off one RECNXINTERVAL and go back to IDLE, where the connection manager's own
 * beat starts a fresh attempt that asks everybody again (the E71 resilience
 * path, unchanged). This node was not a member before and is not one now.
 */
static void join_attempt_exhausted(struct cnxman_join *j)
{
	j->attempts_exhausted++;
	join_backoff_start(j);
	join_stopped(j, CNXMAN_JOIN_FAIL_UNANSWERED,
		     "%CNXMAN, no cluster member answered this node's membership "
		     "request: this node is NOT a cluster member, and will ask "
		     "again");
}

/*
 * Move this join's dialogue to `c` and re-ask. Every value is the executive's:
 * the system id and the Con.ID are that CSB's, and the body goes out stamped
 * from that CSB's own dialogue counters (E77 -- join_cm_take() clears
 * `burst_on_conn`, because a different connection has carried nothing, and
 * join_emit_cm()'s gate refuses to stamp a dialogue that is not on the Con.ID
 * being sent on).
 *
 * The re-ask itself is join_reoffer_burst(): exactly the p. 2-51 re-offer this
 * FSM already makes, now against a different peer -- MODEL and PARAMS if this
 * connection has not carried them (the per-CSB advertisement mask answers that,
 * so a member the beat already introduced this node to is not told twice), then
 * the op-0x02, which is due because the disk walk of this attempt finished.
 */
static void join_reissue_to(struct cnxman_join *j, struct vms_csb *c,
			    int32_t slot)
{
	join_set_target(j, c, (uint32_t)slot);
	join_cm_take(j, (vms_conid_t)c->cdt_conid);
	j->cm_open = 1u;
	j->admit_answered = 0u;
	j->admit_target_acked = 0u;
	j->admit_silent_beats = 0u;
	j->reissues++;
	join_log(j, "%CNXMAN, re-issuing this node's membership request to "
		    "another cluster member");
	join_reoffer_burst(j);
}

/* The member this join asked has said nothing for long enough. Name it, mark it
 * declined for this attempt, and ask the next one -- or, with nobody left to
 * ask, end the attempt honestly. */
static void join_decline_target(struct cnxman_join *j)
{
	struct vms_csb *next = NULL;
	int32_t slot;

	j->requests_unanswered++;
	if (j->admit_target_acked)
		j->declines_after_ack++;
	join_log_not_answered(j, join_target_csb(j));
	if (j->target_csb >= 0)
		join_slot_decline(j, (uint32_t)j->target_csb);

	slot = join_next_askable(j, &next);
	if (slot < 0) {
		j->reissue_targets_absent++;
		join_attempt_exhausted(j);
		return;
	}
	join_reissue_to(j, next, slot);
}

/*
 * IS A CLUSTER STATE TRANSITION RUNNING RIGHT NOW? A read of the barrier's own
 * record of the transition it is participating in -- one the coordinator really
 * opened, never a guess -- and NOT of this join's state.
 *
 * WHY THE CLOCK STOPS FOR IT. p. 7-30 has a connection manager propose a
 * transition only "if no other Connection Manager has already instituted a
 * cluster state transition", and a membership request is a request to institute
 * one. So while a transition is in progress this node holds its clock instead of
 * asking somebody else: two requests inside one transition is the one way a
 * re-issue could make things WORSE rather than only slower. The hold is
 * counted, so a join that never declines because the cluster is permanently
 * transitioning says so rather than looking idle.
 */
static int join_transition_in_progress(const struct cnxman_join *j)
{
	struct cnxman_transition tr;

	if (j->barrier == NULL)
		return 0;
	return cnxman_barrier_transition(j->barrier, &tr) == 0;
}

/* One beat of the silence clock. Runs only in [ADMIT], where a request has
 * really been made. */
static void join_admit_beat(struct cnxman_join *j)
{
	if (join_transition_in_progress(j)) {
		j->admit_beats_held++;
		return;
	}
	if (!join_admit_request_outstanding(j)) {
		j->admit_silent_beats = 0u;
		return;
	}
	if (j->admit_silent_beats < 0xffu)
		j->admit_silent_beats++;
	if (j->admit_silent_beats < (uint8_t)CNXMAN_JOIN_ADMIT_SILENCE_BEATS)
		return;
	join_decline_target(j);
}

static enum cnxman_join_rx join_h_watch_burst(struct cnxman_join *j,
					      const struct join_ev *e)
{
	uint8_t before = j->state;

	join_reoffer_burst(j);
	if (j->state == (uint8_t)CNXMAN_JOIN_ADMIT)
		join_admit_beat(j);
	if (j->state != before)
		return CNXMAN_JOIN_RX_CONSUMED;   /* the beat moved the drive */
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
		/*
		 * E73: and a member's own op-0x14/op-0x01, whenever it comes.
		 * This node now advertises its identity to every member with
		 * an OPEN connection on the beat -- including before
		 * CLUSTER_START, which is exactly the case that cost the live
		 * cluster VAX1's whole dialogue -- and every member
		 * reciprocates in kind (spec sec 4(o) row 3). Dropping the
		 * reply while sending the question would leave that member's
		 * CSB without the VOTES it really advertised, forever: it does
		 * not repeat. The handler WRITES ONLY THE SENDER'S OWN CSB
		 * from the sender's own bytes and asserts nothing about this
		 * node (INV-6).
		 */
		[CNXMAN_EV_RX_CONFIG]    = join_h_peer_advert,
	},

	/* [DIR ROUND] spec sec 4(L)(a)+(b): our OWN SCS$DIRECTORY client
	 * round, resolving every name before connecting to it. */
	[CNXMAN_JOIN_DIR_ROUND] = {
		[CNXMAN_EV_RX_CONFIG]    = join_h_peer_advert,
		[CNXMAN_EV_DIR_RESULT]   = join_h_dir_result,
		[CNXMAN_EV_CM_ACCEPTED]  = join_h_cm_accepted,
		[CNXMAN_EV_TIMER_JOIN]   = join_h_watch_lookup,
	},

	/* [MSCP CONNECT] spec sec 4(L)(c). */
	[CNXMAN_JOIN_MSCP_CONNECT] = {
		[CNXMAN_EV_RX_CONFIG]    = join_h_peer_advert,
		[CNXMAN_EV_CDT_OPEN]     = join_h_mscp_opened,
		[CNXMAN_EV_CM_ACCEPTED]  = join_h_cm_accepted,
		[CNXMAN_EV_CDT_CLOSED]   = join_h_closed,
		[CNXMAN_EV_TIMER_JOIN]   = join_h_watch,
	},

	/*
	 * [VC CONNECT] spec sec 4(L)(d), and -- since E71 -- the one place this
	 * join waits for connectivity to its member, whether that connection
	 * has never been made, could not be put on the wire, or was made and
	 * lost. The beat is p. 7-30's "attempt once a second"; the member's own
	 * connect is adopted here (p. 7-24 REACCEPT); the bound is the CSB's.
	 */
	[CNXMAN_JOIN_VC_CONNECT] = {
		[CNXMAN_EV_RX_CONFIG]    = join_h_peer_advert,
		[CNXMAN_EV_CDT_OPEN]     = join_h_cm_opened,
		[CNXMAN_EV_CM_ACCEPTED]  = join_h_cm_accepted,
		[CNXMAN_EV_CDT_CLOSED]   = join_h_closed,
		[CNXMAN_EV_TIMER_JOIN]   = join_h_watch_vc,
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
		[CNXMAN_EV_TRANSITION_DONE] = join_h_transition_done,
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
		[CNXMAN_EV_TRANSITION_DONE] = join_h_transition_done,
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
		/* A member barriers through OTHER nodes' transitions too, and
		 * each of those commits. The handler is idempotent and reports
		 * the ones whose nodemap did not name us. */
		[CNXMAN_EV_TRANSITION_DONE] = join_h_transition_done,
		[CNXMAN_EV_CDT_CLOSED]   = join_h_closed,
	},

	/*
	 * [FAILED] terminal and honest: nothing here revives a join. A new one
	 * is a new CLUSTER_START, which re-inits this FSM.
	 *
	 * E71 narrowed WHO GETS HERE to a VERDICT -- the peer's REJECT of our
	 * VMS$VAXcluster connect (p. 2-25's version gate), a member that hosts
	 * no connection manager at all, and this node's own codec refusing to
	 * build. A connectivity fact never lands in this row again: it lands in
	 * [VC CONNECT], which is alive, or back in [IDLE], which is waiting.
	 */
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
	case CNXMAN_EV_TRANSITION_DONE:
		return (uint32_t)e->aux_class;   /* WHICH class committed */
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
 * DID THE MEMBER THIS NODE ASKED TAKE THE REQUEST? (E80)
 *
 * Both facts below are read off a REAL inbound body and the CSB it really
 * arrived on -- the executive's own answer to "who is at the other end of this
 * connection" (p. 7-23) -- and neither is ever inferred from a timer, from the
 * join's own state, or from a frame that came from somebody else.
 * ========================================================================== */

/* Did this body arrive on the connection to the member this join is driving
 * through? join_target_csb() re-validates that the slot still holds that SYSTEM
 * (a CSB is deallocated and rebuilt when a system returns, p. 7-25), so a stale
 * slot number cannot make another member's frame look like the target's. */
static int join_ev_from_target(struct cnxman_join *j, const struct join_ev *e)
{
	return e->from_csb >= 0 && j->target_valid &&
	       e->from_csb == j->target_csb && join_target_csb(j) != NULL;
}

/*
 * WHICH events mean "this member has taken the membership request": its
 * op-0x03 COMMIT (the proposal itself, sec 4(o) row 5), the op-0x05 rebuild
 * transactions that follow it, the op-0x06 membership records, and the
 * transition family the proposal opens. Any one of them is a coordinator doing
 * the job, so the silence clock stops.
 *
 * DELIBERATELY NOT HERE: CNXMAN_EV_RX_CONFIG (a member advertises its identity
 * to every peer it has a connection to, joining or not -- E73) and
 * CNXMAN_EV_RX_CLOSE (the recurring member poll). Counting either would call a
 * member that answers nothing an answering coordinator, which is exactly the
 * live case this rule exists for.
 */
static int join_ev_is_admission_progress(enum cnxman_event ev)
{
	switch (ev) {
	case CNXMAN_EV_RX_COMMIT:
	case CNXMAN_EV_RX_MEMBERSHIP:
	case CNXMAN_EV_RX_TR_OPEN:
	case CNXMAN_EV_RX_TR_GO:
	case CNXMAN_EV_RX_BARRIER:
	case CNXMAN_EV_RX_BARRIER_ACK:
	case CNXMAN_EV_RX_REBUILD:
		return 1;
	default:
		return 0;
	}
}

static void join_note_admission_progress(struct cnxman_join *j,
					 enum cnxman_event ev,
					 const struct join_ev *e)
{
	if (!join_ev_is_admission_progress(ev))
		return;
	if (!join_ev_from_target(j, e))
		return;
	j->admit_answered = 1u;
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
	/* A drive really started iff this FSM LEFT idle for something that is
	 * not a refusal. Staying in IDLE is the deferral (join_start_deferred):
	 * nothing was asserted and the caller should ask again. */
	if (j->state == (uint8_t)CNXMAN_JOIN_IDLE ||
	    j->state == (uint8_t)CNXMAN_JOIN_FAILED)
		return -1;
	return 0;
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
			 const uint8_t *body, uint32_t len)
{
	struct join_ev e;

	if (j == NULL || body == NULL)
		return;
	join_bzero(&e, (uint32_t)sizeof(e));
	e.conid = conid;
	e.body = body;
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

enum cnxman_join_rx cnxman_join_rx_body(struct cnxman_join *j,
					const uint8_t *body, uint32_t len,
					vms_csid_t from_csid, int from_valid,
					int32_t from_csb)
{
	struct join_ev e;
	enum cnxman_event ev;

	if (j == NULL || j->cl == NULL || body == NULL)
		return CNXMAN_JOIN_RX_BAD;

	join_bzero(&e, (uint32_t)sizeof(e));
	e.from_csb = from_csb;
	/*
	 * A body that does not parse never reaches the table, so without this
	 * record it would be indistinguishable from a message that never
	 * arrived (E69). `aux` carries its length -- E73 was decoded from
	 * exactly that: three ARRIVAL records reading `unparsed aux=0x84`
	 * (132) were VAX1's op-0x01, VAX2's op-0x01 and VAX2's op-0x03
	 * membership COMMIT, refused by a frame-absolute parser handed a
	 * SYSAP body.
	 */
	if (vms_cm_envelope_parse(body, len, &e.env) != VMS_CODEC_OK) {
		join_diag_arrival(j, CNXMAN_DIAG_EV_NONE,
				  CNXMAN_DIAG_R_UNPARSED, 0, len);
		return CNXMAN_JOIN_RX_BAD;
	}

	e.body = body;
	e.len = len;
	e.from_csid = from_csid;
	e.from_valid = from_valid;

	/*
	 * The ack side of the dialogue is NOT recorded here (E73). ack-msg# is
	 * a fact about the CONNECTION a message arrived on, and this FSM can
	 * only resolve the ONE peer it is joining through -- which left every
	 * other member's block un-acked once this node started holding a real
	 * dialogue with all of them. vms_cnxman.c records it for the CSB the
	 * Con.ID really resolves to, once, before any FSM is offered the body.
	 */

	/* The opportunistic cat-0x04 ack the member sends is a fact, not a
	 * transition: counted, never answered (spec sec 4(u)). */
	if (e.env.category == VMS_CM_CAT_ACK) {
		j->peer_acks++;
		/*
		 * E80: an ack from the member this node asked is recorded as
		 * what it is -- that member is hearing us -- and it does NOT
		 * stop the admission clock. A coordinator acks in ~0.3 ms and
		 * proposes a few ms later; E73's VAX1 acked all run and never
		 * proposed. Keeping the two apart is what lets a stalled join
		 * say WHICH of the two happened.
		 */
		if (join_ev_from_target(j, &e))
			j->admit_target_acked = 1u;
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

	/* E80: recorded BEFORE the dispatch, so it is a fact about the frame
	 * that arrived rather than about what a handler did with it. */
	join_note_admission_progress(j, ev, &e);

	{
		uint32_t before = join_barrier_commits(j);
		enum cnxman_join_rx rx = join_dispatch(j, ev, &e);

		join_post_commit(j, before, &e);
		return rx;
	}
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
