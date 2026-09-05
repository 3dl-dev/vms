/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman_csb.h - the CLUB/CSB model and the CSB ten-state connectivity
 * machine (FC-P3.6).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.4 (the CLUB/CSB data
 * model), SS3.9 (three files per layer; table-driven FSMs with injected ops).
 * The STRUCTS are in vms_cluster.h -- they are the node's cluster data model,
 * read by $GETSYI, SHOW CLUSTER, the quorum arithmetic and the DLM. This header
 * is their behaviour: allocate, find, walk the ten states, project a view.
 *
 * ---------------------------------------------------------------------------
 * GROUNDING (clean-room, published description, page cites only)
 *
 * *VAXcluster Principles* (Davis 1993) SS7.9, pp. 7-23/7-24: "each CSB contains a
 * field reflecting the state of the SCS connection between the local
 * SYS$CLUSTER and the SYS$CLUSTER residing in the system associated with the
 * CSB. There are ten of these 'connectivity' states" -- NEW, CONNECT, ACCEPT,
 * OPEN, DISCONNECT, WAIT, RECONNECT, REACCEPT, DEAD, LOCAL, each of which the
 * book then describes in a paragraph. Those ten are enum vms_cnxman_csb_state
 * in vms_cluster_snapshot.h, and the table in vms_cnxman_csb.c walks exactly
 * them: no eleventh state was invented, and no state was merged away.
 *
 * The transition table is therefore SHAPED by the book, not by a capture. Where
 * the book describes a state but does not say what leaves it, the table's entry
 * is marked INFERRED in the .c and the reason is written next to it. Where the
 * book says nothing at all, the cell is empty and the event is IGNORED (and
 * counted), which is the honest answer -- not a guessed edge.
 * ---------------------------------------------------------------------------
 *
 * INV-6. This machine never manufactures an identity. A CSB slot that is not
 * in_use is not "system zero"; a csid without csid_valid is not "csid 0"; the
 * peer's VOTES/LOCKDIRWT are absent until a real record supplies them, and the
 * projection blanks the column rather than printing the scalar's zero.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_CNXMAN_CSB_H
#define OVMX_VMS_CNXMAN_CSB_H

#include "vms_cluster.h"
#include "vms_cluster_snapshot.h"
#include "vms_cnxman.h"   /* struct cnxman_ops -- the injected clock and log */

/* ==========================================================================
 * 1. The events that move a CSB between the ten states
 *
 * One event per fact the connection manager can OBSERVE about the SCS
 * connection to one remote connection manager. They are facts, not frames: the
 * codec decides which frame means which fact, so an opcode re-mapping after a
 * capture is a table edit somewhere else entirely.
 * ========================================================================== */
enum cnxman_csb_event {
	/* We sent the initial SCS CONNECT to a newly discovered CM (p. 7-24
	 * CONNECT). */
	CNXMAN_CSB_EV_CONNECT_SENT   = 0,

	/* An inbound SCS CONNECT is being accepted. From NEW that is the
	 * initial connect (p. 7-24 ACCEPT); from WAIT/RECONNECT it is the
	 * peer's RECONNECT request and we are the accepting side (REACCEPT). */
	CNXMAN_CSB_EV_CONNECT_RCVD   = 1,

	/* The SCS connection now exists (p. 7-24 OPEN, "the normal state"). */
	CNXMAN_CSB_EV_CONN_OPEN      = 2,

	/* An SCS DISCONNECT was started on an open connection (p. 7-24
	 * DISCONNECT) -- an orderly close, not a failure. */
	CNXMAN_CSB_EV_DISCONNECT     = 3,

	/* Connectivity was lost for a reason NOT involving a last gasp
	 * (p. 7-30). This is the event that starts the reconnect window and
	 * HOLDS membership. */
	CNXMAN_CSB_EV_CONN_LOST      = 4,

	/* The peer ANNOUNCED its departure: a last-gasp datagram (p. 7-29,
	 * BUGCHECK or SHUTDOWN.COM). The port closes the circuit at once and
	 * there is nothing to wait for. */
	CNXMAN_CSB_EV_LAST_GASP      = 5,

	/* The once-a-second beat fired and a reconnect attempt is now in
	 * progress (p. 7-24 RECONNECT, p. 7-30 "once a second"). */
	CNXMAN_CSB_EV_RECNX_ATTEMPT  = 6,

	/* That attempt did not connect; the timeout resumes (p. 7-24 WAIT,
	 * "This will be repeated until ..."). */
	CNXMAN_CSB_EV_RECNX_FAILED   = 7,

	/* The whole reconnect period elapsed (p. 7-30 "if all such reconnect
	 * attempts fail"). */
	CNXMAN_CSB_EV_RECNX_EXPIRED  = 8,

	/* A NEW INCARNATION of that system has been seen (p. 7-24 DEAD: "The
	 * CSB whose connection state is DEAD represents the old incarnation"). */
	CNXMAN_CSB_EV_NEW_INCARNATION = 9,

	/*
	 * The remote connection manager REJECTED a connect this ladder issued
	 * (E81). Distinct from RECNX_FAILED, and the distinction is the whole
	 * point: RECNX_FAILED is "the attempt did not connect", which p. 7-30
	 * answers by attempting again a second later; a REJECT is the peer's own
	 * ANSWER (p. 2-25 / correction D12), and re-asking a peer that answered
	 * is a loop rather than a recovery -- cnxman_join_rejected() already says
	 * so in as many words for the join's own connect. Until E81 nothing
	 * carried that answer to this ladder at all, so the once-a-second beat
	 * re-asked, and the SECOND CONNECT_REQ inside one second bugchecked the
	 * reference VAX's connection manager (CNXMGRERR, 15/15 in
	 * join-e80refire-1788563452).
	 */
	CNXMAN_CSB_EV_CONNECT_REJECTED = 10,

	CNXMAN_CSB_EV__COUNT
};

/* ==========================================================================
 * 2. What a transition asks the caller to DO
 *
 * The machine mutates the CSB and returns the one action its caller owes the
 * cluster. It never sends: a reconnect is an SCS connect the glue issues, and a
 * state transition is the transition FSM's business (FC-P3.12). Keeping the act
 * out here is what lets the whole ladder be unit-tested with no wire at all.
 * ========================================================================== */
enum cnxman_csb_action {
	CNXMAN_CSB_ACT_NONE               = 0,
	CNXMAN_CSB_ACT_RECONNECT          = 1,  /* issue a reconnect to this CM */
	CNXMAN_CSB_ACT_PROPOSE_TRANSITION = 2,  /* start a state transition (p. 7-30) */

	/*
	 * Emit this node's own last-gasp datagram (p. 7-29). NEVER produced by
	 * the state table -- it is node-level, produced only by
	 * cnxman_recnx_shutdown(). It is listed in this one enum so the whole
	 * connection manager has ONE action vocabulary.
	 *
	 * The datagram itself is a PORT-level frame (wire spec SS4(O.30): a
	 * multicast HELLO with the departure marker at abs 30 and the cluster
	 * nonce at abs 68), so the bytes are built by the port, which owns that
	 * frame class and its grounded field map -- never here.
	 */
	CNXMAN_CSB_ACT_LAST_GASP          = 3,

	CNXMAN_CSB_ACT__COUNT
};

/* ==========================================================================
 * 3. The state machine
 *
 * One call, one event, one CSB. `club` is passed because two transitions ask a
 * CLUSTER-WIDE question the CSB cannot answer -- p. 7-30's "if no other
 * Connection Manager has already instituted a cluster state transition" -- and
 * the answer must come from the executive's real transition state, not from a
 * caller's opinion.
 *
 * `ops` supplies the clock (ops->now_ms) and the %CNXMAN console line
 * (ops->log). Both may be absent members; a NULL ops, club or csb returns
 * CNXMAN_CSB_ACT_NONE and changes nothing.
 * ========================================================================== */
enum cnxman_csb_action cnxman_csb_dispatch(struct vms_club *club,
					   struct vms_csb *csb,
					   enum cnxman_csb_event ev,
					   const struct cnxman_ops *ops);

/* How many events the ladder ignored because the book names no such edge.
 * Instrumentation, counted in the CLUB (design SS3.9 rule 3: no globals): a
 * rising count in the lab means a real transition is missing from the table,
 * which is a question for a capture, not a licence to guess an edge. */
uint32_t cnxman_club_ignored_events(const struct vms_club *club);

/* ==========================================================================
 * 4. The CLUB: allocate, find, count
 * ========================================================================== */

/*
 * Make `cl->club` a CLUB. Zeroes it, then creates the LOCAL CSB from the SYSGEN
 * parameters already loaded into cl->params (p. 7-26: the CLUB holds the CSB
 * associated with the local system; p. 7-24: state LOCAL "is reserved for the
 * CSB representing the local Connection Manager"). The local CSID is NOT set --
 * the cluster assigns it and this node learns it.
 * Returns the local CSB, or NULL if `cl` is NULL.
 */
struct vms_csb *cnxman_club_init(struct vms_cluster *cl);

/* The local system's CSB (p. 7-26), or NULL before cnxman_club_init(). */
struct vms_csb *cnxman_club_local(struct vms_club *club);

/*
 * Allocate a CSB for a newly discovered connection manager, in state NEW
 * (p. 7-23). `sysid_valid` decides whether the SCSSYSTEMID is recorded or left
 * honestly absent. Returns NULL when the table is full -- the caller must treat
 * that as a refusal to admit, never as a silent overwrite.
 */
struct vms_csb *cnxman_club_alloc_csb(struct vms_club *club,
				      vms_scs_sysid_t sysid, int sysid_valid);

/* Release a CSB slot (p. 7-25: on a rejoin "its old CSB is deallocated, and a
 * new CSB is created for it just as if it were joining the cluster for the
 * first time"). */
void cnxman_club_free_csb(struct vms_club *club, struct vms_csb *csb);

/* Find by identity. Both skip free slots and both refuse to match on a value
 * the CSB has not LEARNED (a CSB with csid_valid == 0 never matches any CSID,
 * including 0). NULL when there is none. */
struct vms_csb *cnxman_club_find_sysid(struct vms_club *club,
				       vms_scs_sysid_t sysid);
struct vms_csb *cnxman_club_find_csid(struct vms_club *club, vms_csid_t csid);

/* Walk the table by slot: NULL past the high-water mark or on a free slot. */
struct vms_csb *cnxman_club_csb_at(struct vms_club *club, uint32_t index);

/* The slot index of a CSB, or VMS_CLUB_MAX_CSB if it is not in this CLUB. */
uint32_t cnxman_club_csb_index(const struct vms_club *club,
			       const struct vms_csb *csb);

/*
 * Recompute the member count into club->cluster_nodes, p. 7-49: "The total
 * number of members (excluding the quorum disk) is stored in the CLUB. This is
 * simply the total number of CSBs that have their SELECTED flag set." Returns
 * the count. Called at a transition's Phase 2, NOT during a reconnect window --
 * membership is HELD across a break (p. 7-30) and the count must not dip.
 */
uint32_t cnxman_club_recount_members(struct vms_club *club);

/* ==========================================================================
 * 5. Learning -- the only way a peer's advertised value enters a CSB
 *
 * Each setter records BOTH the value and the fact that it was learned, so a
 * field can never be read back as an asserted zero (INV-6). The join and
 * barrier FSMs call these from parsed records; nothing else writes a CSB's
 * identity or parameters.
 * ========================================================================== */
void cnxman_csb_set_csid(struct vms_csb *csb, vms_csid_t csid);
void cnxman_csb_set_sysid(struct vms_csb *csb, vms_scs_sysid_t sysid);
void cnxman_csb_set_scsnode(struct vms_csb *csb, const uint8_t *name, uint8_t len);
void cnxman_csb_set_params(struct vms_csb *csb, uint16_t votes,
			   uint16_t expected_votes, uint16_t qdskvotes);
void cnxman_csb_set_lockdirwt(struct vms_csb *csb, uint8_t lockdirwt);

/*
 * The port-dependent number the REMOTE connection manager supplies for the
 * reconnect timeout (p. 7-30). Absent until the remote actually supplies it;
 * see cnxman_recnx_period_secs() for what absence means.
 */
void cnxman_csb_set_remote_port_secs(struct vms_csb *csb, uint32_t secs);

/* This node's own CSID, learned by matching our SCSSYSTEMID in the membership
 * records. Also stamps the local CSB, which is the same fact. */
void cnxman_club_learn_local_csid(struct vms_club *club, vms_csid_t csid);

/* CSB status flags (VMS_CSB_F_*, p. 7-23). */
void cnxman_csb_set_flags(struct vms_csb *csb, uint16_t flags);
void cnxman_csb_clear_flags(struct vms_csb *csb, uint16_t flags);
int  cnxman_csb_is_member(const struct vms_csb *csb);

/* ==========================================================================
 * 6. Projections -- what SHOW CLUSTER, $GETSYI and CLUSTER_DIAG_CSB read
 *
 * Read-only, taken by the glue under the fork mutex. Every field is copied from
 * a real CSB/CLUB field; a value that was never learned travels with its
 * `_valid` clear so the reader blanks the column (snapshot rule 2).
 * ========================================================================== */
void cnxman_csb_project(const struct vms_csb *csb, struct vms_csb_view *out);
void cnxman_club_project(const struct vms_club *club,
			 enum vms_cluster_state state,
			 struct vms_club_view *out);

/* ==========================================================================
 * 7. Names -- SDA's own spelling, so a lab comparison is a string match
 * ========================================================================== */
const char *cnxman_csb_state_name(enum vms_cnxman_csb_state s);
const char *cnxman_csb_event_name(enum cnxman_csb_event e);
const char *cnxman_csb_action_name(enum cnxman_csb_action a);

/* ==========================================================================
 * 8. The SYSAP envelope stamper (design sec 3.2.4 ruling E1)
 *
 * "72-79 -> CNXMAN via ONE pure stamper cnxman_envelope_stamp(csb, body,
 * is_response) on the CSB's dialogue counters." Every CNXMAN emitter -- the
 * barrier FSM (FC-P3.5), the coordinator (FC-P3.12), and the DLM's cat-02 arm's
 * replies (vms_dlm_scs.h RULE A) -- calls this ONE function for body[0:8], and
 * NOTHING else in a CNXMAN TU writes those eight bytes: the same discipline
 * FC-P0.1's `dlm_scs_role_ops` already applies to a lock grant (a SYSAP that
 * fills its own dialogue counters is the same category error as a daemon that
 * fills a lock id).
 * ========================================================================== */

/*
 * Stamp body[0:8] of a 132-byte CNXMAN SYSAP body from `csb`'s real dialogue
 * state.
 *
 *   body[0:2] send_msg, body[2:4] ack_msg -- THIS node's own counters for the
 *     dialogue with `csb`'s system, written on EVERY call, response or
 *     origination.
 *
 *   body[4:6] txn, body[6:8] token -- written ONLY when `is_response` is 0: an
 *     ORIGINATION carries this CSB's own transaction id and correlation
 *     token (the token's derivation is UNKNOWN per spec sec 4(j); it is
 *     never computed, only carried). When `is_response` is nonzero these two
 *     bytes are left EXACTLY as they already are in `body` -- the answer a
 *     real VMS peer correlates by is the txn/token IT sent, and every
 *     response recipe in vms_cluster_codec_cm.h (the echo family, the
 *     close/DLM-op0d/body/step-ack builders) already puts that echoed value
 *     there before this is called.
 *
 *     The GO and the RELEASE (op 0x0a/0x0c) are notifications that force
 *     txn=0 (spec sec 4(p): "Notifications carry txn=0 and are NEVER
 *     answered") while still carrying a REAL token -- a wire fact about
 *     those two opcodes, not about CSB dialogue state, so it is NOT this
 *     function's business: their builders call this stamper with
 *     is_response=0 (a genuine origination, real txn AND token) and then
 *     `vms_cm_notification_zero_txn()` forces body[4:6] back to zero
 *     afterward (vms_cluster_codec_cm.h). This stamper stays exactly the
 *     two cases above; it does not grow a third.
 *
 * `csb` NULL stamps nothing (the caller's own "no CSB for that destination"
 * refusal has already happened before this is reached; there is no wire
 * counter to read from a system this node has no block for).
 */
void cnxman_envelope_stamp(const struct vms_csb *csb,
			   uint8_t body[132] /* VMS_CM_BODY_LEN */,
			   int is_response);

/*
 * The two dialogue cells the stamper reads, maintained as NAMED CSB
 * operations rather than by an emitter incrementing another layer's field
 * (FC-P3.3). Both rules are spec sec 4(j)'s own:
 *
 *   cnxman_csb_dialogue_sent()  -- call ONCE per body actually handed to SCS
 *      for this peer, BEFORE stamping it, so the first message carries
 *      send-msg# 1 ("strictly monotonic per sender ... starts at 1 on the
 *      first VC message", 2902/2902 golden frames).
 *
 *   cnxman_csb_dialogue_heard() -- call with the send-msg# of every body that
 *      really ARRIVED from this peer. It keeps the MAXIMUM, because ack-msg#
 *      "acknowledges the peer's highest send-msg# seen": a retransmit
 *      legitimately repeats a lower number and must not walk the ack back.
 */
void cnxman_csb_dialogue_sent(struct vms_csb *csb);
void cnxman_csb_dialogue_heard(struct vms_csb *csb, uint16_t peer_send_msg);

/*
 * THE COUNTERS BELONG TO A CONNECTION, NOT TO A SYSTEM (E77).
 *
 * cnxman_csb_bind_connection() records the VMS$VAXcluster Con.ID the executive
 * holds for this system -- it is the ONLY writer of `csb->cdt_conid` -- and,
 * when that Con.ID is a DIFFERENT one, restarts the dialogue: send-msg# back to
 * 0 so the next origination carries 1, ack-msg# back to 0 so this node acks
 * nothing until the peer really sends on the new connection.
 *
 * WHY. Spec sec 4(j) grounds send-msg# as "starts at 1 on the first VC
 * message", and the golden wire measures the restart directly: in
 * vax3-2to3-established-join-20260730 the station at send-msg# 21078 on Con.ID
 * pair 3551000a/a4980009 opens 18e3000a/a498000d at send_msg=1 ack=0, and in
 * formation-ci1 the SAME station pair's SECOND dialogue (3359000a/63080008)
 * opens at send_msg=1 ack=0 after 17541 messages on the first. Per-system
 * continuation is not what a real node does.
 *
 * WHAT IT COST. Keeping the counters per-CSB across teardowns, while
 * join_emit_cm() legitimately BURNS a number on a refused send, made this node
 * open brand-new Con.IDs at send-msg# 8 and 13 -- acking peer messages the peer
 * had never sent on them. Both real VAXes bugchecked (CNXMGRERR, "Error
 * detected by VAXcluster Connection Manager") within 1.2 ms and 0.2 ms of those
 * two bursts and left the cluster (integration notes E76/E77). The burn is
 * still correct WITHIN a connection: a gap, never a repeat. It simply cannot
 * cross a teardown, because the number it burned belonged to a dialogue that no
 * longer exists.
 *
 * cnxman_csb_dialogue_is_on() is the same rule read the other way, for an
 * emitter that sends on a Con.ID it holds itself rather than on `cdt_conid`:
 * nonzero only when this block's dialogue state IS that connection's.
 */
void cnxman_csb_bind_connection(struct vms_csb *csb, uint32_t conid);
int  cnxman_csb_dialogue_is_on(const struct vms_csb *csb, uint32_t conid);

/*
 * ASSIGN this dialogue's next send-msg# and STAMP the body with it, in that
 * order -- the two calls above, fused, because doing them in the other order
 * or in two different places is how a duplicate got onto the wire (E73).
 *
 * THE DEFECT THIS CLOSES. The join advanced the counter itself before
 * stamping (correct: its first message to a peer carries 1), while the
 * barrier and the coordinator stamped first and let vms_cnxman.c's transport
 * thunks advance AFTER the send. Both are individually monotonic, but they are
 * one apart in PHASE -- and they share a CSB, because the member a join is
 * driven through is the coordinator whose transition the barrier then answers.
 * So the first barrier-side body stamped after the join's last one repeated
 * its send-msg#, on the one connection that matters, at the exact moment the
 * admission dialogue hands over to the transition. Spec sec 4(j) measured
 * "strictly monotonic per sender" over 17 539 of 17 541 golden frames (the two
 * residuals are retransmits, which repeat a number deliberately); a fresh
 * origination that repeats one is not something a real node does.
 *
 * EVERY origination and every response now goes through this one function, and
 * the transport thunks advance nothing. `is_response` is the stamper's own
 * flag: a response leaves the echoed txn/token the codec already copied.
 */
void cnxman_envelope_originate(struct vms_csb *csb,
			       uint8_t body[132] /* VMS_CM_BODY_LEN */,
			       int is_response);

#endif /* OVMX_VMS_CNXMAN_CSB_H */
