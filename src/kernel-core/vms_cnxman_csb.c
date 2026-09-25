/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman_csb.c - the CLUB/CSB model and the CSB ten-state connectivity
 * machine (FC-P3.6).
 *
 * The structures are in vms_cluster.h; the contract, the grounding and the
 * INV-6 rules are in vms_cnxman_csb.h. This file is the behaviour: one table
 * indexed [state][event], one small handler per edge, and the CLUB bookkeeping
 * around it.
 *
 * READ THE TABLE, NOT THE PROSE. csb_table[][] below IS the specification of
 * this machine. Every populated cell cites the page that puts the edge there;
 * three cells are marked INFERRED with the reason; every empty cell is an event
 * the published description does not connect to that state, and is ignored and
 * COUNTED rather than guessed.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 * This TU is PURE: no seam call, no allocation, no clock but ops->now_ms -- so
 * it runs identically in both kmods, in the host unit tests and in the rung-2
 * N-node simulator.
 */

#include "vms_cluster.h"
#include "vms_cluster_snapshot.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_recnx_fsm.h"   /* the p. 7-30 period arithmetic + constants */

/* ==========================================================================
 * Small shared helpers
 * ========================================================================== */

/* Byte-wise zero. This TU calls no library and no seam primitive (the substrate
 * spells memset differently and a pure TU must build on the host too), and a CSB
 * is a few dozen bytes cleared at most once per discovered system. */
static void csb_bzero(void *p, uint32_t n)
{
	uint8_t *b = (uint8_t *)p;
	uint32_t i;

	for (i = 0; i < n; i++)
		b[i] = 0u;
}

static void csb_zero(struct vms_csb *csb)
{
	csb_bzero(csb, (uint32_t)sizeof(*csb));
}

/* The %CNXMAN console line, if the caller gave us somewhere to put it. */
static void csb_log(const struct cnxman_ops *ops, const char *msg)
{
	if (ops != NULL && ops->log != NULL)
		ops->log(ops->ctx, msg);
}

/* The injected clock. Zero when no clock was injected -- which is a caller
 * error, not a licence to read the substrate's (design SS3.9 rule 6). */
static uint32_t csb_now(const struct cnxman_ops *ops)
{
	if (ops != NULL && ops->now_ms != NULL)
		return ops->now_ms(ops->ctx);
	return 0u;
}

/* ==========================================================================
 * The give-up ledger (rd vms-0f9). Three small functions, one writer.
 * ========================================================================== */

static struct vms_club_giveup *giveup_find(struct vms_club *club,
					   vms_scs_sysid_t sysid)
{
	uint32_t i;

	for (i = 0; i < (uint32_t)VMS_CLUB_MAX_GIVEUP; i++) {
		if (club->giveup[i].in_use && club->giveup[i].sysid == sysid)
			return &club->giveup[i];
	}
	return (struct vms_club_giveup *)0;
}

/*
 * RECORD IT. One record per system -- a second give-up on the same system
 * overwrites the incarnation rather than accumulating, because the question
 * this ledger answers is always about the CURRENT incarnation in front of us.
 *
 * A give-up we cannot name an incarnation for is NOT recorded: a record with
 * no incarnation could not tell the old incarnation from the new one, and the
 * connect would then be refused forever (INV-6: no record, no refusal).
 */
static void giveup_arm(struct vms_club *club, const struct vms_csb *csb)
{
	struct vms_club_giveup *g;
	uint32_t i;

	if (club == NULL || csb == NULL)
		return;
	if (!csb->sysid_valid || !csb->incarnation_valid)
		return;

	g = giveup_find(club, csb->sysid);
	if (g == (struct vms_club_giveup *)0) {
		for (i = 0; i < (uint32_t)VMS_CLUB_MAX_GIVEUP; i++) {
			if (!club->giveup[i].in_use) {
				g = &club->giveup[i];
				break;
			}
		}
	}
	if (g == (struct vms_club_giveup *)0) {
		club->giveup_overflow++;
		return;
	}
	g->sysid = csb->sysid;
	g->incarnation = csb->incarnation;
	g->in_use = 1u;
	club->giveup_armed++;
}

/* p. 7-25's edge: the system came back as somebody else. */
static void giveup_clear(struct vms_club *club, vms_scs_sysid_t sysid)
{
	struct vms_club_giveup *g = giveup_find(club, sysid);

	if (g == (struct vms_club_giveup *)0)
		return;
	g->in_use = 0u;
	g->sysid = 0u;
	g->incarnation = 0u;
	club->giveup_cleared++;
}

int cnxman_club_gave_up_on(const struct vms_club *club, vms_scs_sysid_t sysid,
			   uint64_t incarnation)
{
	uint32_t i;

	if (club == NULL)
		return 0;
	for (i = 0; i < (uint32_t)VMS_CLUB_MAX_GIVEUP; i++) {
		if (!club->giveup[i].in_use)
			continue;
		if (club->giveup[i].sysid != sysid)
			continue;
		return club->giveup[i].incarnation == incarnation ? 1 : 0;
	}
	return 0;
}

void cnxman_club_giveup_arm(struct vms_club *club, const struct vms_csb *csb)
{
	giveup_arm(club, csb);
}

uint32_t cnxman_club_giveup_count(const struct vms_club *club)
{
	uint32_t i, n = 0u;

	if (club == NULL)
		return 0u;
	for (i = 0; i < (uint32_t)VMS_CLUB_MAX_GIVEUP; i++) {
		if (club->giveup[i].in_use)
			n++;
	}
	return n;
}

void cnxman_csb_set_incarnation(struct vms_club *club, struct vms_csb *csb,
				uint64_t incarnation, int valid)
{
	if (csb == NULL)
		return;
	if (!valid) {
		csb->incarnation = 0u;
		csb->incarnation_valid = 0u;
		return;
	}
	/*
	 * p. 7-25, DECIDED WHERE BOTH NUMBERS ARE IN SCOPE. A give-up record
	 * that names a DIFFERENT incarnation of this system is about a system
	 * that no longer exists; the one in front of us is new and is dealt
	 * with from scratch.
	 */
	if (club != NULL && csb->sysid_valid &&
	    !cnxman_club_gave_up_on(club, csb->sysid, incarnation))
		giveup_clear(club, csb->sysid);

	csb->incarnation = incarnation;
	csb->incarnation_valid = 1u;
}

/*
 * THE SUBJECT OF A RECONFIGURATION IS A MEMBER (rd vms-b36).
 *
 * p. 7-49 makes SELECTED the cluster's COMMITTED membership -- the member count
 * in the CLUB "is simply the total number of CSBs that have their SELECTED flag
 * set", written at a transition's Phase 2 -- which is exactly why csb_give_up()
 * below refuses to touch it. p. 7-30's reconfiguration is the answer to losing
 * contact with a system that IS in the cluster; a system the cluster never
 * admitted has no membership to remove, and a transition proposing to remove it
 * asserts a membership change that never happened.
 *
 * MEASURED, and it is not theoretical: in
 * tests/lab/captures/vms-b36-cnxmgrerr-20260925/ a second OVMX node's
 * admission was abandoned by the real VAX coordinator ("timed-out lost
 * connection to system OVMXB" -> "aborting VAXcluster state transition"), and
 * 0.6 s later THIS code proposed a cluster reconfiguration removing that same
 * never-admitted system. The real OpenVMS VAX V7.3 took a fatal CNXMGRERR
 * bugcheck. Across the four runs the predecessor lane archived, the console
 * line "proposing removal of a system from the cluster" appears in exactly the
 * one run that bugchecked a peer and in none of the three that did not.
 *
 * THE ORACLE SAYS THE SAME THING. On a three-node cluster of REAL OpenVMS VAX
 * V7.3 nodes (tests/lab/captures/vms-b36-cnxmgrerr-20260925/oracle/), a
 * joiner blacked out mid-admission is ABANDONED, never removed: no surviving
 * member proposes a reconfiguration naming it. When a real MEMBER goes, exactly
 * one member proposes and the others follow.
 *
 * Returns 1 when the caller must NOT propose.
 */
static int csb_nothing_to_remove(struct vms_csb *csb,
				 const struct cnxman_ops *ops)
{
	if ((csb->flags & VMS_CSB_F_SELECTED) != 0u)
		return 0;
	csb->removals_withheld++;
	csb_log(ops, "%CNXMAN, the cluster never admitted this system: giving "
		     "up its connection and proposing no state transition");
	return 1;
}

/*
 * p. 7-30: the local connection manager starts a state transition only "if no
 * other Connection Manager has already instituted a cluster state transition".
 * The answer comes from the CLUB's real transition state -- what this node has
 * actually observed on the wire -- never from a caller's opinion.
 *
 * Deferring leaves the CSB where it is, so the next once-a-second tick asks
 * again: if the running transition removes the peer, this CSB is gone; if the
 * transition ends without removing it, we propose then.
 */
static enum cnxman_csb_action csb_propose_or_defer(struct vms_club *club,
						   struct vms_csb *csb,
						   const struct cnxman_ops *ops)
{
	if (club->transition_active) {
		csb_log(ops, "%CNXMAN, transition already in progress, deferring");
		return CNXMAN_CSB_ACT_NONE;
	}
	/* Counted as PROPOSED, not as occurred: club->last_transition_ms is
	 * p. 7-26's "when the last state transition occurred" and belongs to
	 * whoever completes one (FC-P3.5/FC-P3.12), not to whoever asks. */
	csb->transitions_proposed++;
	return CNXMAN_CSB_ACT_PROPOSE_TRANSITION;
}

/*
 * Give up on a connection: state DISCONNECT and this node's own view of the
 * peer's membership is cleared.
 *
 * VMS_CSB_F_SELECTED is deliberately NOT touched. p. 7-49 makes SELECTED the
 * cluster's committed membership -- the member count in the CLUB "is simply the
 * total number of CSBs that have their SELECTED flag set", written at a
 * transition's Phase 2 -- so only a transition may move it. If a lost
 * connection cleared it here, CLUSTER_NODES would drop before the cluster had
 * agreed that anything left.
 */
static void csb_give_up(struct vms_club *club, struct vms_csb *csb)
{
	/*
	 * ...AND THE LEDGER REMEMBERS WHICH INCARNATION (rd vms-0f9). This is
	 * the moment this node stops dealing with the system in front of it,
	 * and p. 7-24's DEAD state says the executive is expected to know which
	 * incarnation that was. giveup_arm() records nothing it cannot name.
	 */
	giveup_arm(club, csb);

	csb->state = (uint8_t)VMS_CNXMAN_CSB_DISCONNECT;
	csb->flags &= (uint16_t)~VMS_CSB_F_MEMBER;
	csb->next_attempt_ms = 0u;
	csb->deadline_ms = 0u;

	/*
	 * AND THE CON.ID CLAIM GOES WITH IT (rd vms-dfe), for the reason E81
	 * already gives on the reject path: `cdt_conid` is this block's claim to
	 * HOLD a connection to that system, and a block that has given up on the
	 * connection does not hold one. A stale claim is not inert -- it is read
	 * by join_cm_take_held() as "the executive already holds this pair's
	 * connection, open none of your own", by join_cm_sync_with_csb() as a
	 * connection to adopt, and by the emitters as an envelope to stamp for a
	 * CDT that does not exist (the E76/E77 crash family). Released through
	 * the single writer, which also discards the dialogue that died with it.
	 */
	cnxman_csb_bind_connection(csb, 0u);
}

/* ==========================================================================
 * The transition handlers -- one edge each
 * ========================================================================== */

/* p. 7-24 CONNECT: "The initial SCS CONNECT request has been sent to the newly
 * discovered remote Connection Manager." */
static enum cnxman_csb_action h_connect_sent(struct vms_club *club,
					     struct vms_csb *csb,
					     const struct cnxman_ops *ops)
{
	(void)club; (void)ops;
	csb->state = (uint8_t)VMS_CNXMAN_CSB_CONNECT;
	return CNXMAN_CSB_ACT_NONE;
}

/* p. 7-24 ACCEPT: "An initial SCS CONNECT request from a newly discovered
 * remote Connection Manager is being accepted." */
static enum cnxman_csb_action h_accept(struct vms_club *club,
				       struct vms_csb *csb,
				       const struct cnxman_ops *ops)
{
	(void)club; (void)ops;
	csb->state = (uint8_t)VMS_CNXMAN_CSB_ACCEPT;
	return CNXMAN_CSB_ACT_NONE;
}

/* p. 7-24 REACCEPT: "The local Connection Manager is accepting a reconnect
 * request from the remote Connection Manager represented by the CSB." The peer
 * beat our own once-a-second attempt to it; membership stays held. */
static enum cnxman_csb_action h_reaccept(struct vms_club *club,
					 struct vms_csb *csb,
					 const struct cnxman_ops *ops)
{
	(void)club; (void)ops;
	csb->state = (uint8_t)VMS_CNXMAN_CSB_REACCEPT;
	return CNXMAN_CSB_ACT_NONE;
}

/*
 * p. 7-24 OPEN: "An SCS connection exists ... This is the normal state of a
 * CSB." Reached from a fresh connect/accept, or from the reconnect apparatus,
 * in which case the break is over and is counted.
 *
 * Note what this does NOT do: it does not make the peer a member. An open SCS
 * connection is connectivity, and membership is a cluster decision taken by a
 * state transition (p. 7-28/7-49). Conflating the two is how a node "joins" a
 * cluster that never admitted it.
 */
static enum cnxman_csb_action h_open(struct vms_club *club,
				     struct vms_csb *csb,
				     const struct cnxman_ops *ops)
{
	(void)club; (void)ops;
	if (csb->state == (uint8_t)VMS_CNXMAN_CSB_WAIT ||
	    csb->state == (uint8_t)VMS_CNXMAN_CSB_RECONNECT ||
	    csb->state == (uint8_t)VMS_CNXMAN_CSB_REACCEPT)
		csb->reconnects++;

	csb->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;
	csb->lost_ms = 0u;
	csb->deadline_ms = 0u;
	csb->next_attempt_ms = 0u;
	csb->attempts = 0u;
	return CNXMAN_CSB_ACT_NONE;
}

/* p. 7-24 DISCONNECT: "An SCS DISCONNECT is in progress for an open SCS
 * connection". An ORDERLY close of our own; no reconnect window, and no
 * transition proposed from here -- the caller closing a connection knows why. */
static enum cnxman_csb_action h_disconnect(struct vms_club *club,
					   struct vms_csb *csb,
					   const struct cnxman_ops *ops)
{
	(void)club; (void)ops;
	/* NOT a give-up on the peer's incarnation (rd vms-0f9): this is our own
	 * orderly close and the caller knows why. Passing NULL leaves the
	 * ledger alone rather than refusing that system's next connect. */
	csb_give_up((struct vms_club *)0, csb);
	return CNXMAN_CSB_ACT_NONE;
}

/*
 * p. 7-24 WAIT + p. 7-30. Connectivity was lost for a reason not involving a
 * last gasp: "Do not presume that the remote system has left, or will be
 * leaving the cluster simply because the local Connection Manager has lost
 * contact". So: enter WAIT, start the timeout, and HOLD membership -- the
 * MEMBER flag is deliberately untouched here, and that is the whole point of
 * the reconnect apparatus.
 *
 * The timeout is sized here because the CSB is where p. 7-30's two inputs live:
 * this node's RECNXINTERVAL (copied into the CLUB from SYSGEN at init) and the
 * number the REMOTE connection manager supplied for this port.
 */
static enum cnxman_csb_action h_conn_lost(struct vms_club *club,
					  struct vms_csb *csb,
					  const struct cnxman_ops *ops)
{
	uint32_t now = csb_now(ops);
	uint32_t period_secs = cnxman_recnx_period_secs(club->recnxinterval,
							csb->remote_port_valid
								? csb->remote_port_secs
								: 0u);

	csb->state = (uint8_t)VMS_CNXMAN_CSB_WAIT;
	csb->lost_ms = now;
	csb->deadline_ms = now + (period_secs * 1000u);
	csb->next_attempt_ms = now + CNXMAN_RECNX_ATTEMPT_MS;
	csb->attempts = 0u;
	csb_log(ops, "%CNXMAN, lost connection to a cluster member, reconnecting");
	return CNXMAN_CSB_ACT_NONE;
}

/*
 * p. 7-29: a last-gasp datagram (BUGCHECK, or SHUTDOWN.COM) is a DEPARTURE
 * ANNOUNCEMENT. "When an SCA port driver receives such a datagram, it
 * immediately closes the virtual circuit ... Assuming quorum has not been lost,
 * the Connection Manager in one of the remaining systems will initiate a
 * cluster state transition to reconfigure the cluster." There is nothing to
 * wait for: no WAIT, no reconnect, no timeout.
 */
static enum cnxman_csb_action h_last_gasp(struct vms_club *club,
					  struct vms_csb *csb,
					  const struct cnxman_ops *ops)
{
	csb_give_up(club, csb);
	csb_log(ops, "%CNXMAN, received last gasp from a cluster member");
	/* A departure announcement from a system the cluster never admitted is
	 * still an announcement -- the connection goes -- but there is no
	 * membership to reconfigure away (rd vms-b36). */
	if (csb_nothing_to_remove(csb, ops))
		return CNXMAN_CSB_ACT_NONE;
	return csb_propose_or_defer(club, csb, ops);
}

/* p. 7-24 RECONNECT + p. 7-30's "attempt once a second". The beat fired: an
 * attempt is now in progress. Membership is still held. */
static enum cnxman_csb_action h_recnx_attempt(struct vms_club *club,
					      struct vms_csb *csb,
					      const struct cnxman_ops *ops)
{
	(void)club;
	csb->state = (uint8_t)VMS_CNXMAN_CSB_RECONNECT;
	csb->attempts++;
	csb->next_attempt_ms = csb_now(ops) + CNXMAN_RECNX_ATTEMPT_MS;
	return CNXMAN_CSB_ACT_RECONNECT;
}

/*
 * THE PEER ANSWERED "NO" (E81). p. 2-25 / correction D12: a REJECT is the
 * remote connection manager's JUDGEMENT on the 16-byte connect data, not a lost
 * attempt -- and p. 7-30's "attempt once a second" is the rule for attempts that
 * get no answer. Re-asking a peer that answered is the loop
 * cnxman_join_rejected() already refuses for the join's own connect, and it is
 * measured: on join-e80refire-1788563452 this ladder's reconnect was rejected,
 * the reject reached nobody, and the beat re-asked 1.024 s later -- 15 of 15
 * times, that SECOND VMS$VAXcluster CONNECT_REQ was followed ~470 ms later by
 * the reference VAX's CNXMGRERR bugcheck and last gasp.
 *
 * SO: no further attempt is made in THIS reconnect window. The window itself is
 * untouched -- `deadline_ms` was set when connectivity was lost and is not
 * extended or shortened here -- so p. 7-30's ending is unchanged: the window
 * runs out and h_recnx_expired() makes its conditional proposal. And the peer's
 * own reconnect is still taken, because [WAIT] keeps its CONNECT_RCVD ->
 * h_reaccept edge (p. 7-24 REACCEPT); this node stops ASKING, it does not stop
 * ANSWERING.
 *
 * MEMBERSHIP IS HELD, exactly as h_conn_lost holds it: nothing about the remote
 * system's membership changed because it refused one connect (p. 7-30, "do not
 * presume that the remote system has left").
 */
static enum cnxman_csb_action h_connect_rejected(struct vms_club *club,
						 struct vms_csb *csb,
						 const struct cnxman_ops *ops)
{
	(void)club;
	csb->connect_rejects++;
	csb->state = (uint8_t)VMS_CNXMAN_CSB_WAIT;
	csb->next_attempt_ms = csb->deadline_ms;
	csb_log(ops, "%CNXMAN, a cluster member refused this node's reconnect: "
		     "not asking it again until the reconnect interval expires");
	return CNXMAN_CSB_ACT_NONE;
}

/*
 * THE PEER TORE IT DOWN ITSELF (rd vms-dfe). The contract and the measured
 * 2 Hz loop this closes are in vms_cnxman_csb.h at
 * CNXMAN_CSB_EV_REMOTE_DISCONNECT; the BEHAVIOUR is deliberately identical to
 * h_connect_rejected's, because the two events are the same fact about the
 * peer -- it answered -- arriving by two different verbs.
 *
 * TWO POSITIONS, ONE ANSWER:
 *
 *   - FROM [OPEN]: the window has to START, exactly as h_conn_lost starts it
 *     (this is the first loss), and then no attempt is made inside it.
 *   - FROM [WAIT]/[RECONNECT]/[REACCEPT]: the window is already running and is
 *     NOT extended -- the deadline set at the first loss is what p. 7-30 ends.
 *
 * MEMBERSHIP IS HELD either way, for h_conn_lost's own reason: "do not presume
 * that the remote system has left ... simply because the local Connection
 * Manager has lost contact" (p. 7-30). A system that disconnected one SYSAP
 * connection has not left the cluster, and only a state transition may say it
 * has.
 */
static enum cnxman_csb_action h_remote_disconnect(struct vms_club *club,
						  struct vms_csb *csb,
						  const struct cnxman_ops *ops)
{
	if (csb->state == (uint8_t)VMS_CNXMAN_CSB_OPEN)
		(void)h_conn_lost(club, csb, ops);   /* start p. 7-30's window */

	csb->remote_disconnects++;
	csb->state = (uint8_t)VMS_CNXMAN_CSB_WAIT;
	csb->next_attempt_ms = csb->deadline_ms;   /* no attempt in THIS window */
	if (csb->remote_disconnects == 1u)
		csb_log(ops, "%CNXMAN, a cluster member disconnected this "
			     "node's VMS$VAXcluster connection: not dialling it "
			     "again until the reconnect interval expires");
	return CNXMAN_CSB_ACT_NONE;
}

/* p. 7-24 WAIT: "This will be repeated until either connectivity is once again
 * established ... or a time limit is exceeded". The attempt failed, so the
 * timeout resumes; the deadline set at loss time is NOT extended. */
static enum cnxman_csb_action h_recnx_failed(struct vms_club *club,
					     struct vms_csb *csb,
					     const struct cnxman_ops *ops)
{
	(void)club; (void)ops;
	csb->state = (uint8_t)VMS_CNXMAN_CSB_WAIT;
	return CNXMAN_CSB_ACT_NONE;
}

/*
 * p. 7-30: "If all such 'reconnect' attempts fail, and if no other Connection
 * Manager has already instituted a cluster state transition, then the local
 * Connection Manager starts a state transition to reconfigure the cluster."
 * The suppression is real: on a deferral the CSB stays where it is and the next
 * tick asks again.
 */
static enum cnxman_csb_action h_recnx_expired(struct vms_club *club,
					      struct vms_csb *csb,
					      const struct cnxman_ops *ops)
{
	enum cnxman_csb_action act;

	/*
	 * The window is over either way -- p. 7-30 is done attempting, so the
	 * block gives up its connection and its Con.ID claim. WHETHER A
	 * TRANSITION IS PROPOSED is the separate question, and a system this
	 * cluster never admitted is not the subject of one (rd vms-b36).
	 */
	if (csb_nothing_to_remove(csb, ops)) {
		csb_give_up(club, csb);
		return CNXMAN_CSB_ACT_NONE;
	}

	act = csb_propose_or_defer(club, csb, ops);

	/* Deferred: another connection manager is already reconfiguring the
	 * cluster, so this CSB stays exactly where it is and the next
	 * once-a-second tick asks the question again. */
	if (act == CNXMAN_CSB_ACT_NONE)
		return act;

	csb_give_up(club, csb);
	csb_log(ops, "%CNXMAN, reconnect interval expired, proposing removal");
	return act;
}

/* p. 7-24 DEAD: "A new incarnation of a VAX system has been seen. The CSB whose
 * connection state is DEAD represents the old incarnation." The CLUB keeps it
 * until the caller deallocates it and builds a fresh CSB for the new
 * incarnation (p. 7-25). */
static enum cnxman_csb_action h_dead(struct vms_club *club,
				     struct vms_csb *csb,
				     const struct cnxman_ops *ops)
{
	(void)club;
	csb->state = (uint8_t)VMS_CNXMAN_CSB_DEAD;
	csb->flags &= (uint16_t)~VMS_CSB_F_MEMBER;
	csb_log(ops, "%CNXMAN, new incarnation seen, old CSB is dead");
	return CNXMAN_CSB_ACT_NONE;
}

/*
 * INFERRED (not a book edge). An INITIAL connect that never completes: the
 * connection was never OPEN, so p. 7-30's reconnect apparatus -- which is about
 * a LOST connection and about not presuming a member departed -- does not
 * apply, and there is no membership to hold. The book names no "failed initial
 * connect" state, and NEW is its own description of the resting place: "The CSB
 * has just been allocated. It can represent a newly discovered remote
 * Connection Manager". So the CSB returns there and the discovery may be
 * retried. The alternative -- leaving it in CONNECT forever -- is a wedge.
 */
static enum cnxman_csb_action h_connect_abandoned(struct vms_club *club,
						  struct vms_csb *csb,
						  const struct cnxman_ops *ops)
{
	(void)club; (void)ops;
	csb->state = (uint8_t)VMS_CNXMAN_CSB_NEW;
	return CNXMAN_CSB_ACT_NONE;
}

/*
 * The peer's REJECT of this node's INITIAL connect (E81). There is no reconnect
 * window here and no membership to hold, so h_connect_abandoned's own grounding
 * applies unchanged -- "the connection was never OPEN, so p. 7-30's reconnect
 * apparatus ... does not apply" -- and the CSB rests in NEW. The reject is
 * COUNTED, because "the peer refuses us" and "the peer never answered" are the
 * two diagnoses an operator has to be able to tell apart.
 */
static enum cnxman_csb_action h_initial_connect_rejected(struct vms_club *club,
							 struct vms_csb *csb,
							 const struct cnxman_ops *ops)
{
	csb->connect_rejects++;
	csb_log(ops, "%CNXMAN, a cluster member refused this node's connection");
	return h_connect_abandoned(club, csb, ops);
}

/* ==========================================================================
 * The table. [state][event]; NULL = the published description names no such
 * edge, so the event is ignored and counted.
 * ========================================================================== */
typedef enum cnxman_csb_action (*csb_handler_t)(struct vms_club *,
						struct vms_csb *,
						const struct cnxman_ops *);

static const csb_handler_t csb_table[VMS_CNXMAN_CSB_STATE__COUNT]
				    [CNXMAN_CSB_EV__COUNT] = {
	/* [NEW] a freshly allocated CSB: either side may open the connection. */
	[VMS_CNXMAN_CSB_NEW] = {
		[CNXMAN_CSB_EV_CONNECT_SENT]    = h_connect_sent,
		[CNXMAN_CSB_EV_CONNECT_RCVD]    = h_accept,
		[CNXMAN_CSB_EV_NEW_INCARNATION] = h_dead,
	},

	/* [CONNECT] our initial CONNECT is outstanding. */
	[VMS_CNXMAN_CSB_CONNECT] = {
		[CNXMAN_CSB_EV_CONN_OPEN]       = h_open,
		[CNXMAN_CSB_EV_CONN_LOST]       = h_connect_abandoned, /* INFERRED */
		[CNXMAN_CSB_EV_CONNECT_REJECTED] = h_initial_connect_rejected,
		[CNXMAN_CSB_EV_DISCONNECT]      = h_disconnect,
		[CNXMAN_CSB_EV_NEW_INCARNATION] = h_dead,
	},

	/* [ACCEPT] we are accepting their initial CONNECT. */
	[VMS_CNXMAN_CSB_ACCEPT] = {
		[CNXMAN_CSB_EV_CONN_OPEN]       = h_open,
		[CNXMAN_CSB_EV_CONN_LOST]       = h_connect_abandoned, /* INFERRED */
		[CNXMAN_CSB_EV_DISCONNECT]      = h_disconnect,
		[CNXMAN_CSB_EV_NEW_INCARNATION] = h_dead,
	},

	/* [OPEN] the normal state: everything that can go wrong, goes wrong
	 * from here. */
	[VMS_CNXMAN_CSB_OPEN] = {
		[CNXMAN_CSB_EV_DISCONNECT]      = h_disconnect,
		[CNXMAN_CSB_EV_CONN_LOST]       = h_conn_lost,
		[CNXMAN_CSB_EV_REMOTE_DISCONNECT] = h_remote_disconnect,
		[CNXMAN_CSB_EV_LAST_GASP]       = h_last_gasp,
		[CNXMAN_CSB_EV_NEW_INCARNATION] = h_dead,
	},

	/* [DISCONNECT] a disconnect in progress, and where a departed system's
	 * retained CSB rests (p. 7-25) until a new incarnation is seen or the
	 * CLUB deallocates it. */
	[VMS_CNXMAN_CSB_DISCONNECT] = {
		[CNXMAN_CSB_EV_NEW_INCARNATION] = h_dead,
	},

	/* [WAIT] the p. 7-30 timeout is running. */
	[VMS_CNXMAN_CSB_WAIT] = {
		[CNXMAN_CSB_EV_RECNX_ATTEMPT]   = h_recnx_attempt,
		/* E81: a reject can land here when the beat has already stepped
		 * the CSB back to WAIT under the outstanding attempt. */
		[CNXMAN_CSB_EV_CONNECT_REJECTED] = h_connect_rejected,
		[CNXMAN_CSB_EV_REMOTE_DISCONNECT] = h_remote_disconnect,
		[CNXMAN_CSB_EV_RECNX_EXPIRED]   = h_recnx_expired,
		[CNXMAN_CSB_EV_CONNECT_RCVD]    = h_reaccept,
		[CNXMAN_CSB_EV_CONN_OPEN]       = h_open,
		[CNXMAN_CSB_EV_LAST_GASP]       = h_last_gasp,
		[CNXMAN_CSB_EV_NEW_INCARNATION] = h_dead,
	},

	/* [RECONNECT] our attempt is in flight. The once-a-second beat keeps
	 * firing from here too: p. 7-30's cadence runs for the whole period,
	 * not just for the first attempt. */
	[VMS_CNXMAN_CSB_RECONNECT] = {
		[CNXMAN_CSB_EV_RECNX_ATTEMPT]   = h_recnx_attempt,
		[CNXMAN_CSB_EV_CONNECT_REJECTED] = h_connect_rejected,
		[CNXMAN_CSB_EV_REMOTE_DISCONNECT] = h_remote_disconnect,
		[CNXMAN_CSB_EV_CONN_OPEN]       = h_open,
		[CNXMAN_CSB_EV_RECNX_FAILED]    = h_recnx_failed,
		[CNXMAN_CSB_EV_RECNX_EXPIRED]   = h_recnx_expired,
		[CNXMAN_CSB_EV_CONNECT_RCVD]    = h_reaccept,
		[CNXMAN_CSB_EV_LAST_GASP]       = h_last_gasp,
		[CNXMAN_CSB_EV_NEW_INCARNATION] = h_dead,
	},

	/* [REACCEPT] we are accepting THEIR reconnect. The timeout keeps
	 * running underneath it -- the deadline was set when the connection was
	 * lost and this state does not extend it. */
	[VMS_CNXMAN_CSB_REACCEPT] = {
		[CNXMAN_CSB_EV_CONN_OPEN]       = h_open,
		[CNXMAN_CSB_EV_REMOTE_DISCONNECT] = h_remote_disconnect,
		[CNXMAN_CSB_EV_RECNX_FAILED]    = h_recnx_failed,
		[CNXMAN_CSB_EV_RECNX_EXPIRED]   = h_recnx_expired,
		[CNXMAN_CSB_EV_LAST_GASP]       = h_last_gasp,
		[CNXMAN_CSB_EV_NEW_INCARNATION] = h_dead,
	},

	/* [DEAD] the old incarnation. Nothing revives it: p. 7-25 says the old
	 * CSB is DEALLOCATED and a new one built for the returning system. */
	[VMS_CNXMAN_CSB_DEAD] = { 0 },

	/* [LOCAL] p. 7-24: reserved for the local connection manager's own CSB.
	 * It has no SCS connection to itself, so no connectivity event applies
	 * -- and the reconnect loop must never be able to propose this node's
	 * own removal. */
	[VMS_CNXMAN_CSB_LOCAL] = { 0 },
};

enum cnxman_csb_action cnxman_csb_dispatch(struct vms_club *club,
					   struct vms_csb *csb,
					   enum cnxman_csb_event ev,
					   const struct cnxman_ops *ops)
{
	csb_handler_t h;

	if (club == NULL || csb == NULL || !csb->in_use)
		return CNXMAN_CSB_ACT_NONE;
	if ((unsigned)ev >= (unsigned)CNXMAN_CSB_EV__COUNT)
		return CNXMAN_CSB_ACT_NONE;
	if ((unsigned)csb->state >= (unsigned)VMS_CNXMAN_CSB_STATE__COUNT)
		return CNXMAN_CSB_ACT_NONE;

	h = csb_table[csb->state][ev];
	if (h == NULL) {
		club->csb_ignored_events++;
		return CNXMAN_CSB_ACT_NONE;
	}
	return h(club, csb, ops);
}

uint32_t cnxman_club_ignored_events(const struct vms_club *club)
{
	return (club != NULL) ? club->csb_ignored_events : 0u;
}

/* ==========================================================================
 * The CLUB
 * ========================================================================== */

struct vms_csb *cnxman_club_init(struct vms_cluster *cl)
{
	struct vms_club *club;
	struct vms_csb *local;

	if (cl == NULL)
		return NULL;

	club = &cl->club;
	csb_bzero(club, (uint32_t)sizeof(*club));
	club->local_csb = -1;   /* -1, not 0: slot 0 is a real slot */

	/*
	 * p. 7-30's local half of the reconnect period, copied from the SYSGEN
	 * parameters STARTUP.EXE loaded before CLUSTER_START. Zero means SYSGEN
	 * never carried it; the published OpenVMS default (20 s, the same value
	 * tools/vms_sysgen.c's parameter table carries) stands in, because a
	 * zero-second reconnect period would give up on a member instantly --
	 * the opposite of what p. 7-30 requires.
	 */
	club->recnxinterval = cl->params.recnxinterval;
	if (club->recnxinterval == 0u) {
		club->recnxinterval = CNXMAN_RECNX_DEFAULT_SECS;
		club->recnxinterval_defaulted = 1u;
	}

	local = cnxman_club_alloc_csb(club, cl->params.scssystemid, 1);
	if (local == NULL)
		return NULL;

	/* p. 7-24: state LOCAL; p. 7-23: the flag saying this CSB is us. */
	local->state = (uint8_t)VMS_CNXMAN_CSB_LOCAL;
	local->flags |= (uint16_t)VMS_CSB_F_LOCAL;
	cnxman_csb_set_scsnode(local, cl->params.scsnode, cl->params.scsnode_len);
	cnxman_csb_set_params(local, cl->params.votes, cl->params.expected_votes,
			      cl->params.qdskvotes);
	cnxman_csb_set_lockdirwt(local, cl->params.lockdirwt);
	club->local_csb = (int32_t)cnxman_club_csb_index(club, local);
	return local;
}

struct vms_csb *cnxman_club_local(struct vms_club *club)
{
	if (club == NULL || club->local_csb < 0)
		return NULL;
	if ((uint32_t)club->local_csb >= (uint32_t)VMS_CLUB_MAX_CSB)
		return NULL;
	return club->csb[club->local_csb].in_use ? &club->csb[club->local_csb]
						 : NULL;
}

struct vms_csb *cnxman_club_alloc_csb(struct vms_club *club,
				      vms_scs_sysid_t sysid, int sysid_valid)
{
	struct vms_csb *csb;
	uint32_t i;

	if (club == NULL)
		return NULL;

	for (i = 0; i < (uint32_t)VMS_CLUB_MAX_CSB; i++) {
		if (club->csb[i].in_use)
			continue;
		csb = &club->csb[i];
		csb_zero(csb);
		csb->in_use = 1u;
		/* p. 7-23 NEW: "The CSB has just been allocated." */
		csb->state = (uint8_t)VMS_CNXMAN_CSB_NEW;
		if (sysid_valid) {
			csb->sysid = sysid;
			csb->sysid_valid = 1u;
		}
		if (i + 1u > club->n_csb)
			club->n_csb = i + 1u;
		return csb;
	}
	/* Full. The caller must refuse the admission, never overwrite a CSB. */
	return NULL;
}

void cnxman_club_free_csb(struct vms_club *club, struct vms_csb *csb)
{
	if (club == NULL || csb == NULL || !csb->in_use)
		return;
	if (club->local_csb >= 0 &&
	    csb == &club->csb[club->local_csb])
		club->local_csb = -1;
	csb_zero(csb);
}

/* ==========================================================================
 * THE DEALLOCATION THE TABLE'S OWN [DISCONNECT] ROW ALREADY PROMISED (vms-dfe)
 *
 * p. 7-25: on a return, "its old CSB is deallocated, and a new CSB is created
 * for it just as if it were joining the cluster for the first time"; p. 7-24
 * DEAD is the old incarnation's block, kept "until the caller deallocates it
 * and builds a fresh CSB for the new incarnation". Both sentences describe a
 * deallocation that this file named and NOTHING in the executive performed:
 * cnxman_club_free_csb() had no production caller at all.
 *
 * WHAT ITS ABSENCE COST, MEASURED (the rd vms-dfe in-browser stall,
 * tests/lab/captures/vms-e18e-cn3-browser-20260925/cn3-intermittent/). A
 * joiner's VMS$VAXcluster connection to the one cluster member dropped before
 * it was admitted; the p. 7-30 window ran out; h_recnx_expired() parked the
 * block in DISCONNECT. From there the table offers exactly one edge
 * (NEW_INCARNATION), csb_ensure()/cnxman_discover_peers() find the block by
 * SCSSYSTEMID and never allocate another, and join_askable() -- correctly --
 * refuses to drive a join through a connection the ladder has given up on. So
 * a node whose PE circuit to that member was still OPEN, still exchanging
 * HELLOs, could never speak to it again for the life of the boot: 25 minutes
 * of "waiting to form or join an OpenVMS Cluster" beside a healthy cluster.
 * Real VMS does not have that resting place; it deallocates and rebuilds.
 *
 * WHAT IS RECLAIMED, and why each guard is a READ and not a policy:
 *
 *   - STATE. Only DISCONNECT and DEAD -- the two states whose own book text is
 *     "this connection is over". Every other state is a live ladder position.
 *   - NOT THE LOCAL BLOCK. p. 7-24 LOCAL is this system's own CSB and has no
 *     SCS connection to give up on; freeing it would unmake the CLUB.
 *   - NOT A SELECTED BLOCK. p. 7-49 makes SELECTED the cluster's COMMITTED
 *     membership ("the total number of CSBs that have their SELECTED flag
 *     set"), and only a state transition may move it (csb_give_up's own note).
 *     A member the cluster has not yet removed keeps its block, so
 *     CLUSTER_NODES cannot dip because a connection blinked.
 *   - NOT A BLOCK STILL CLAIMING A CON.ID. `cdt_conid` is the executive's
 *     record that a connection exists; a block that still claims one is a block
 *     SCS can still call back about, and freeing it would lose the attribution.
 *     (csb_give_up() releases the claim, so the normal give-up path qualifies
 *     on the very next sweep; an ORDERLY disconnect still in flight does not.)
 *
 * NOTHING IS ASSERTED. This frees a table slot and no more: it opens no
 * connection, sends nothing, and makes no claim about the system the block
 * described. Whether a new block appears is the PORT's answer, not this
 * function's -- cnxman_discover_peers() reallocates one only for a system SCS
 * really reports an open circuit to (INV-6).
 * ========================================================================== */
static int csb_reclaimable(const struct vms_club *club,
			   const struct vms_csb *csb, uint32_t slot)
{
	if (!csb->in_use)
		return 0;
	if (club->local_csb >= 0 && (uint32_t)club->local_csb == slot)
		return 0;
	if ((csb->flags & (VMS_CSB_F_LOCAL | VMS_CSB_F_SELECTED)) != 0u)
		return 0;
	if (csb->cdt_conid != 0u)
		return 0;
	return csb->state == (uint8_t)VMS_CNXMAN_CSB_DISCONNECT ||
	       csb->state == (uint8_t)VMS_CNXMAN_CSB_DEAD;
}

uint32_t cnxman_club_reclaim_abandoned(struct vms_club *club,
				       vms_scs_sysid_t *released, uint32_t max)
{
	uint32_t i, n = 0u;

	if (club == NULL)
		return 0u;
	for (i = 0; i < club->n_csb; i++) {
		struct vms_csb *csb = &club->csb[i];

		if (!csb_reclaimable(club, csb, i))
			continue;
		/*
		 * WHO was released, so the caller can tell whatever was still
		 * driving through that system. Only a sysid the block really
		 * LEARNED is reported (INV-6); a block that never carried one
		 * is freed silently because there is nothing to name, and the
		 * slot still counts in club->csb_reclaimed.
		 *
		 * A FULL ARRAY STOPS THE SWEEP rather than freeing blocks it
		 * cannot name: releasing a system and not telling the caller is
		 * exactly the half-run-attempt-on-a-rebuilt-block hazard this
		 * out-parameter exists to prevent. The caller drains the rest by
		 * calling again, which is why the batch may be small enough to
		 * live on a VAX kernel stack.
		 */
		if (csb->sysid_valid) {
			if (released == NULL || n >= max)
				break;
			released[n] = csb->sysid;
			n++;
		}
		cnxman_club_free_csb(club, csb);
		club->csb_reclaimed++;
	}
	return n;
}

struct vms_csb *cnxman_club_find_sysid(struct vms_club *club,
				       vms_scs_sysid_t sysid)
{
	uint32_t i;

	if (club == NULL)
		return NULL;
	for (i = 0; i < club->n_csb; i++) {
		if (!club->csb[i].in_use || !club->csb[i].sysid_valid)
			continue;
		if (club->csb[i].sysid == sysid)
			return &club->csb[i];
	}
	return NULL;
}

struct vms_csb *cnxman_club_find_csid(struct vms_club *club, vms_csid_t csid)
{
	uint32_t i;

	if (club == NULL)
		return NULL;
	for (i = 0; i < club->n_csb; i++) {
		/* A CSB whose CSID has not been LEARNED matches nothing --
		 * including a lookup for CSID 0 (INV-6). */
		if (!club->csb[i].in_use || !club->csb[i].csid_valid)
			continue;
		if (club->csb[i].csid == csid)
			return &club->csb[i];
	}
	return NULL;
}

struct vms_csb *cnxman_club_csb_at(struct vms_club *club, uint32_t index)
{
	if (club == NULL || index >= club->n_csb)
		return NULL;
	return club->csb[index].in_use ? &club->csb[index] : NULL;
}

uint32_t cnxman_club_csb_index(const struct vms_club *club,
			       const struct vms_csb *csb)
{
	uint32_t i;

	if (club == NULL || csb == NULL)
		return (uint32_t)VMS_CLUB_MAX_CSB;
	for (i = 0; i < (uint32_t)VMS_CLUB_MAX_CSB; i++) {
		if (&club->csb[i] == csb)
			return i;
	}
	return (uint32_t)VMS_CLUB_MAX_CSB;
}

uint32_t cnxman_club_recount_members(struct vms_club *club)
{
	uint32_t i, n = 0u;

	if (club == NULL)
		return 0u;
	/* p. 7-49: "the total number of CSBs that have their SELECTED flag
	 * set", the local system's CSB included -- it is one of them. */
	for (i = 0; i < club->n_csb; i++) {
		if (club->csb[i].in_use &&
		    (club->csb[i].flags & VMS_CSB_F_SELECTED) != 0u)
			n++;
	}
	club->cluster_nodes = n;
	return n;
}

/* ==========================================================================
 * Learning
 * ========================================================================== */

void cnxman_csb_set_csid(struct vms_csb *csb, vms_csid_t csid)
{
	if (csb == NULL)
		return;
	csb->csid = csid;
	csb->csid_valid = 1u;
}

void cnxman_csb_set_sysid(struct vms_csb *csb, vms_scs_sysid_t sysid)
{
	if (csb == NULL)
		return;
	csb->sysid = sysid;
	csb->sysid_valid = 1u;
}

void cnxman_csb_set_scsnode(struct vms_csb *csb, const uint8_t *name,
			    uint8_t len)
{
	uint8_t i;

	if (csb == NULL || name == NULL)
		return;
	if (len > (uint8_t)VMS_SCSNODE_MAX)
		len = (uint8_t)VMS_SCSNODE_MAX;
	for (i = 0; i < len; i++)
		csb->scsnode[i] = name[i];
	for (; i < (uint8_t)(VMS_SCSNODE_MAX + 2); i++)
		csb->scsnode[i] = 0u;
	csb->scsnode_len = len;
}

/* p. 7-23: the CSB holds "that system's values for the parameters used in the
 * quorum algorithm: VOTES, EXPECTED_VOTES, and QDSKVOTES". */
void cnxman_csb_set_params(struct vms_csb *csb, uint16_t votes,
			   uint16_t expected_votes, uint16_t qdskvotes)
{
	if (csb == NULL)
		return;
	csb->votes = votes;
	csb->expected_votes = expected_votes;
	csb->qdskvotes = qdskvotes;
	csb->params_valid = 1u;
}

/* p. 7-23: "The Connection Manager is responsible for rebuilding the Lock
 * Directory Weight Vector, so each CSB also contains the value of its system's
 * LOCKDIRWT parameter." Absent until FC-P3.2 pins which wire byte carries it
 * for a REMOTE system; the LOCAL CSB gets it from SYSGEN at init. */
void cnxman_csb_set_lockdirwt(struct vms_csb *csb, uint8_t lockdirwt)
{
	if (csb == NULL)
		return;
	csb->lockdirwt = lockdirwt;
	csb->lockdirwt_valid = 1u;
}

void cnxman_csb_set_swver(struct vms_csb *csb, const uint8_t *swver,
			  uint8_t len, const uint8_t *own, uint8_t own_len)
{
	uint32_t i;
	int same;

	if (csb == NULL)
		return;
	if (swver == NULL || len == 0u) {
		for (i = 0; i < (uint32_t)VMS_CLUSTER_SWVER_LEN; i++)
			csb->peer_swver[i] = 0u;
		csb->peer_swver_len = 0u;
		csb->peer_is_ours = 0u;   /* nothing advertised is not proof */
		return;
	}
	if (len > (uint8_t)VMS_CLUSTER_SWVER_LEN)
		len = (uint8_t)VMS_CLUSTER_SWVER_LEN;
	for (i = 0; i < (uint32_t)VMS_CLUSTER_SWVER_LEN; i++)
		csb->peer_swver[i] = (i < (uint32_t)len) ? swver[i] : 0u;
	csb->peer_swver_len = len;

	/* THE DERIVATION, in the one place both tokens are in scope. */
	same = (own != NULL && own_len != 0u && own_len == len);
	for (i = 0; same && i < (uint32_t)len; i++) {
		if (own[i] != swver[i])
			same = 0;
	}
	csb->peer_is_ours = (uint8_t)(same ? 1 : 0);
}

void cnxman_csb_set_remote_port_secs(struct vms_csb *csb, uint32_t secs)
{
	if (csb == NULL)
		return;
	csb->remote_port_secs = secs;
	csb->remote_port_valid = 1u;
}

void cnxman_club_learn_local_csid(struct vms_club *club, vms_csid_t csid)
{
	struct vms_csb *local;

	if (club == NULL)
		return;
	club->local_csid = csid;
	club->local_csid_valid = 1u;
	local = cnxman_club_local(club);
	if (local != NULL)
		cnxman_csb_set_csid(local, csid);
}

void cnxman_csb_set_flags(struct vms_csb *csb, uint16_t flags)
{
	if (csb != NULL)
		csb->flags |= flags;
}

void cnxman_csb_clear_flags(struct vms_csb *csb, uint16_t flags)
{
	if (csb != NULL)
		csb->flags &= (uint16_t)~flags;
}

int cnxman_csb_is_member(const struct vms_csb *csb)
{
	if (csb == NULL || !csb->in_use)
		return 0;
	return (csb->flags & VMS_CSB_F_MEMBER) != 0u;
}

/* ==========================================================================
 * Projections
 * ========================================================================== */

void cnxman_csb_project(const struct vms_csb *csb, struct vms_csb_view *out)
{
	uint32_t i;

	if (out == NULL)
		return;
	csb_bzero(out, (uint32_t)sizeof(*out));
	if (csb == NULL || !csb->in_use)
		return;

	out->csid = csb->csid;
	out->csid_valid = csb->csid_valid;
	out->state = csb->state;
	out->is_member = (uint8_t)((csb->flags & VMS_CSB_F_MEMBER) != 0u);
	out->is_selected = (uint8_t)((csb->flags & VMS_CSB_F_SELECTED) != 0u);
	out->status_rcvd = (uint8_t)((csb->flags & VMS_CSB_F_STATUS_RCVD) != 0u);
	out->scsnode_len = csb->scsnode_len;
	for (i = 0; i < (uint32_t)(VMS_SCSNODE_MAX + 2); i++)
		out->scsnode[i] = csb->scsnode[i];

	/*
	 * An advertised value the peer never sent is NOT projected: the view
	 * keeps the zero it was cleared to, and the flag beside it stays clear,
	 * so the reader blanks the column instead of printing a number nobody
	 * claimed (snapshot rule 2).
	 *
	 * `votes_valid` closes the gap FC-P3.6 flagged here: an un-advertised
	 * VOTES is now distinguishable from an advertised 0 on the far side of
	 * the ioctl (FC-P3.7).
	 */
	if (csb->params_valid) {
		out->votes = csb->votes;
		out->votes_valid = 1u;
	}
	out->lockdirwt = csb->lockdirwt_valid ? csb->lockdirwt : 0u;
	out->lockdirwt_valid = csb->lockdirwt_valid;

	if (csb->sysid_valid) {
		out->peer_sysid_lo = (uint32_t)(csb->sysid & 0xffffffffu);
		out->peer_sysid_hi = (uint32_t)((csb->sysid >> 32) & 0xffffffffu);
	}
	out->sw_version = csb->sw_version;
	out->cdt_conid = csb->cdt_conid;
	out->incarnation_lo = (uint32_t)(csb->incarnation & 0xffffffffu);
	out->incarnation_hi = (uint32_t)((csb->incarnation >> 32) & 0xffffffffu);
	out->last_status_ms = csb->last_status_ms;
}

void cnxman_club_project(const struct vms_club *club,
			 enum vms_cluster_state state,
			 struct vms_club_view *out)
{
	uint32_t i;

	if (out == NULL)
		return;
	csb_bzero(out, (uint32_t)sizeof(*out));
	if (club == NULL)
		return;

	out->local_csid = club->local_csid;
	out->local_csid_valid = club->local_csid_valid;
	out->state = (uint8_t)state;
	out->quorum_lost = club->quorum_lost;
	out->epoch = club->epoch;
	out->cluster_nodes = club->cluster_nodes;
	out->cevotes = club->cevotes;
	out->quorum = club->quorum;
	out->expected_votes = club->expected_votes;
	for (i = 0; i < (uint32_t)VMS_CLUB_BITMAP_WORDS; i++)
		out->bitmap[i] = club->bitmap[i];
	out->bitmap_slots_seen = club->bitmap_slots_seen;
	out->transition_active = club->transition_active;
	out->transition_class = club->transition_class;
	out->barrier_step = club->barrier_step;
	out->coordinator_valid = club->coordinator_valid;
	out->coordinator_csid = club->coordinator_csid;
	out->outstanding_rebuild = club->outstanding_rebuild;
	if (club->ftime_valid) {
		out->ftime_lo = (uint32_t)(club->ftime & 0xffffffffu);
		out->ftime_hi = (uint32_t)((club->ftime >> 32) & 0xffffffffu);
	}
	if (club->fsysid_valid) {
		out->fsysid_lo = (uint32_t)(club->fsysid & 0xffffffffu);
		out->fsysid_hi = (uint32_t)((club->fsysid >> 32) & 0xffffffffu);
	}
	out->reformations = club->reformations;
}

/* ==========================================================================
 * Names -- SDA's spelling, pp. 7-23/7-24
 * ========================================================================== */

static const char *const csb_state_names[VMS_CNXMAN_CSB_STATE__COUNT] = {
	"NEW", "CONNECT", "ACCEPT", "OPEN", "DISCONNECT",
	"WAIT", "RECONNECT", "REACCEPT", "DEAD", "LOCAL"
};

static const char *const csb_event_names[CNXMAN_CSB_EV__COUNT] = {
	"connect sent", "connect received", "connection open",
	"disconnect", "connectivity lost", "last gasp",
	"reconnect attempt", "reconnect failed", "reconnect expired",
	"new incarnation", "connect rejected", "remote disconnect"
};

static const char *const csb_action_names[CNXMAN_CSB_ACT__COUNT] = {
	"none", "reconnect", "propose state transition", "last gasp"
};

const char *cnxman_csb_state_name(enum vms_cnxman_csb_state s)
{
	if ((unsigned)s >= (unsigned)VMS_CNXMAN_CSB_STATE__COUNT)
		return "?";
	return csb_state_names[s];
}

const char *cnxman_csb_event_name(enum cnxman_csb_event e)
{
	if ((unsigned)e >= (unsigned)CNXMAN_CSB_EV__COUNT)
		return "?";
	return csb_event_names[e];
}

const char *cnxman_csb_action_name(enum cnxman_csb_action a)
{
	if ((unsigned)a >= (unsigned)CNXMAN_CSB_ACT__COUNT)
		return "?";
	return csb_action_names[a];
}

/* ==========================================================================
 * The SYSAP envelope stamper (design sec 3.2.4 ruling E1). See the header's
 * "8. The SYSAP envelope stamper" for the contract.
 *
 * The two-byte little-endian write below mirrors vms_wire_put_le16's own
 * convention (vms_cluster_codec.c) but is written locally rather than
 * pulled in from the codec: this stamper's whole span is the fixed,
 * universal 8-byte envelope every `VMS$VAXcluster` body begins with (spec
 * sec 4(j)), never a field whose placement could move with a capture, so it
 * carries its own trivial primitive instead of coupling a CSB-model TU to
 * the wire codec header for four field offsets.
 * ========================================================================== */

#define CSB_STAMP_OFF_SEND_MSG 0u  /* body[0:2] */
#define CSB_STAMP_OFF_ACK_MSG  2u  /* body[2:4] */
#define CSB_STAMP_OFF_TXN      4u  /* body[4:6] */
#define CSB_STAMP_OFF_TOKEN    6u  /* body[6:8] */

static void csb_stamp_put_le16(uint8_t *body, uint32_t off, uint16_t val)
{
	body[off]     = (uint8_t)(val & 0xffu);
	body[off + 1] = (uint8_t)((val >> 8) & 0xffu);
}

/*
 * The next value of a 16-bit cell that is NEVER allowed to read as zero: zero
 * is what "this node asserted nothing here" looks like, and both cells this
 * serves are fields a real node never leaves at zero (vms_cnxman_csb.h SS8).
 * Sixteen bits wrap; the wrap skips 0 and lands on 1, so the sequence is
 * 1,2,...,0xffff,1,... and no cycle ever presents the absent value.
 */
static uint16_t csb_next_nonzero(uint16_t v)
{
	uint16_t n = (uint16_t)(v + 1u);

	return (n == 0u) ? 1u : n;
}

void cnxman_envelope_stamp(const struct vms_csb *csb, uint8_t body[132],
			   int keep_pair)
{
	if (csb == NULL || body == NULL)
		return;

	csb_stamp_put_le16(body, CSB_STAMP_OFF_SEND_MSG, csb->cm_send_msg);
	csb_stamp_put_le16(body, CSB_STAMP_OFF_ACK_MSG, csb->cm_ack_msg);

	if (keep_pair)
		return;   /* the body already carries the pair it should */

	csb_stamp_put_le16(body, CSB_STAMP_OFF_TXN, csb->cm_txn);
	csb_stamp_put_le16(body, CSB_STAMP_OFF_TOKEN, csb->cm_token);
}

/* ==========================================================================
 * The dialogue counters themselves (FC-P3.3)
 *
 * cnxman_envelope_stamp() above READS body[0:4] out of the CSB; these two
 * functions are how those two cells come to hold something true. They exist
 * as named CSB operations for the same reason the stamper does: an emitter
 * that reaches in and does `csb->cm_send_msg++` is doing arithmetic on
 * another layer's dialogue state, which is the same category error as an
 * emitter computing a body offset (design sec 3.9 rule 2).
 *
 * Spec sec 4(j) grounds both cells and both rules:
 *   send-msg#  "strictly monotonic per sender, 2902/2902 golden VC frames
 *              (VAX1: 1,2,3 ... independent of VAX2: 1,2,3 ...). Starts at 1
 *              on the first VC message."  -- so the FIRST message this node
 *              sends to this peer must carry 1, which is what a pre-increment
 *              off a freshly zeroed CSB produces.
 *   ack-msg#   "acknowledges the peer's highest send-msg# seen ... advances
 *              in lockstep with the peer's sends" -- so it is a MAXIMUM over
 *              what really arrived, never a copy of the last frame's field
 *              (a retransmit legitimately repeats a lower number, and 2 of
 *              17 541 golden frames do exactly that).
 * ========================================================================== */

/*
 * ADOPT A CONNECTION FOR THIS SYSTEM, AND START ITS DIALOGUE (E77).
 *
 * The one writer of `cdt_conid`, because "the executive now holds Con.ID X for
 * this system" and "this block's send/ack numbers describe Con.ID X" are the
 * same fact and must never be able to drift apart. Re-binding the SAME Con.ID
 * changes nothing -- the glue writes it on every accept and on every reconnect
 * that returned the connection already held.
 */
void cnxman_csb_bind_connection(struct vms_csb *csb, uint32_t conid)
{
	if (csb == NULL)
		return;

	csb->cdt_conid = conid;
	if (csb->cm_dialogue_conid == conid)
		return;

	/*
	 * A DIFFERENT connection is a DIFFERENT dialogue. Zero is what "nothing
	 * has been said and nothing has been heard here" really is on a
	 * connection this node has not used yet, so the first origination on it
	 * carries send-msg# 1 (pre-increment, spec sec 4(j)) and acks 0 until
	 * this peer really sends something on THIS connection -- never a number
	 * inherited from the dialogue that just died (INV-6).
	 */
	if (csb->cm_dialogue_conid != 0u)
		csb->cm_dialogue_resets++;   /* a LIVE dialogue was discarded */
	csb->cm_dialogue_conid = conid;
	csb->cm_send_msg = 0u;
	csb->cm_ack_msg  = 0u;
	/*
	 * A NEW DIALOGUE TAKES A NEW TRANSACTION ID AND RESTARTS ITS TOKEN
	 * (E85). See vms_cnxman_csb.h SS8 for why these two cells are this
	 * node's own to mint and what is measured about them; the rule here is
	 * only the per-dialogue one: `cm_txn` moves on, so two dialogues on
	 * this block never present the same transaction id, and `cm_token`
	 * restarts so the first origination on a fresh connection carries 1
	 * exactly as send-msg# does.
	 */
	csb->cm_txn = csb_next_nonzero(csb->cm_txn);
	csb->cm_token = 0u;
}

/*
 * Is this block's dialogue state the dialogue of `conid`? An emitter that is
 * about to put a body on a connection asks this before stamping: a body stamped
 * from one connection's counters and transmitted on another asserts a
 * conversation that did not happen, which is the envelope a real VAX bugchecks
 * on (E77). Con.ID 0 is "no connection" and is never a dialogue.
 */
int cnxman_csb_dialogue_is_on(const struct vms_csb *csb, uint32_t conid)
{
	if (csb == NULL || conid == 0u)
		return 0;
	return csb->cm_dialogue_conid == conid;
}

void cnxman_csb_dialogue_sent(struct vms_csb *csb)
{
	if (csb == NULL)
		return;
	csb->cm_send_msg = (uint16_t)(csb->cm_send_msg + 1u);
}

void cnxman_csb_transaction_opened(struct vms_csb *csb)
{
	if (csb == NULL)
		return;
	csb->cm_token = csb_next_nonzero(csb->cm_token);
}

void cnxman_csb_dialogue_heard(struct vms_csb *csb, uint16_t peer_send_msg)
{
	if (csb == NULL)
		return;
	if (peer_send_msg > csb->cm_ack_msg)
		csb->cm_ack_msg = peer_send_msg;
}

void cnxman_envelope_originate(struct vms_csb *csb, uint8_t body[132],
			       enum cnxman_envelope_kind kind)
{
	int mints = (kind == CNXMAN_ENV_REQUEST);

	cnxman_csb_dialogue_sent(csb);
	/*
	 * Only a REQUEST opens a transaction. A RESPONSE carries the peer's
	 * own pair back (the codec already put it in `body`), and a NOTIFY
	 * carries none at all -- in both cases the counter must not move, or a
	 * run of them would consume tokens this node never asserted (E85).
	 */
	if (mints)
		cnxman_csb_transaction_opened(csb);
	cnxman_envelope_stamp(csb, body, !mints);
}
