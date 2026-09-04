/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman_join_fsm.h - the JOIN: the choreography a node runs, as a
 * CLIENT, to be admitted to a running VMScluster (FC-P3.3).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.4 (CLUB/CSB and the
 * LEARNED local CSID), SS3.5 (the SYSINIT ordering CLUSTER_START reproduces),
 * SS3.9 (pure table-driven FSM, injected ops, injected clock, no globals, no
 * raw wire offset outside a codec TU). Wire spec: docs/cluster-protocol-spec.md
 * SS4(L) (the active-joiner drive sequence), SS4(o) (the category-0x01
 * membership dialogue end to end), SS4(j) (the SYSAP body field map), SS4(n)
 * (the disk-client discovery walk), SS4(N) (the 16-byte SCA connect data),
 * SS4(y) (the Rule of Total Connectivity). Book grounding:
 * docs/design-cluster-book-grounding.md SS3.4 and corrections D7, D9, D12.
 *
 * ===========================================================================
 * THE ONE SENTENCE THAT COST THE PREDECESSOR CAMPAIGN THREE MODEL INVERSIONS
 *
 * "The joiner actively DRIVES the post-START sequence; it is not a passive
 * responder" (spec SS4(L)(1)). A node that only answers the member's
 * directory queries and sends its config on the MEMBER-initiated connection
 * is never admitted -- the member times out at ~1.4 s and re-issues START,
 * forever (65 re-issues over 425 s, observed). Everything below is the shape
 * of that drive.
 *
 * ===========================================================================
 * THE CHOREOGRAPHY, IN ORDER, WITH THE GROUNDING FOR EACH STEP
 *
 *  1. OWN SCS$DIRECTORY CONNECT. The joiner opens its own directory
 *     connection as SCS$DIR_LOOKUP -> SCS$DIRECTORY; it does NOT reuse the
 *     member's (SS4(L)(b) "open its own SCS$DIRECTORY CLIENT connection (SCA
 *     idx20, send_seq=1 ...)"; SS4(o)'s af2 window, step 1). One shared
 *     monotonic per-channel send_seq underneath, which is the port's business
 *     and never this file's.
 *
 *  2. LOOKUPS. Ask that connection about each SYSAP the joiner is about to
 *     connect to -- MSCP$DISK and VMS$VAXcluster (SS4(L)(b), SS4(o) step 2).
 *     LOOKUP-BEFORE-CONNECT IS LOAD-BEARING, NOT COSMETIC: firing the
 *     MSCP$DISK connect without first resolving the name occupies a slot in
 *     the shared sequence that the member cannot process, froze the member's
 *     recv_ack at the last directory response, and regressed OVMX below NEW
 *     to blank status (SS4(L), "Shared-sequence deadlock -- the mechanism,
 *     live-grounded"). This FSM cannot violate it: the MSCP$DISK connect is
 *     reachable only from the state a HIT on that name puts it in.
 *
 *  3. MSCP$DISK CLIENT CONNECT, then the discovery walk (SS4(L)(c),
 *     SS4(o) steps 3-4). The walk itself -- SET CONTROLLER CHARACTERISTICS
 *     twice, then the GET UNIT STATUS NEXT-UNIT walk to its Unit-Offline
 *     terminator -- is FC-P3.4's FSM (vms_mscp_cl_fsm.h); this file drives it
 *     and records the units it really enumerated.
 *
 *     IT IS NOT A MEMBERSHIP PREREQUISITE (E68). If the member refuses this
 *     connect or drops it, this node simply enumerates none of that member's
 *     units -- counted, logged -- and the drive goes straight on to step 4.
 *     The reference join measures the two as independent (its VMS$VAXcluster
 *     connection to VAX2 reached OPEN 0.46 s BEFORE it connected MSCP$DISK to
 *     that same member), and treating the refusal as fatal is what left OVMX
 *     silent through a whole live-cluster run. Grounding, in full:
 *     vms_cnxman_join_fsm.c, join_disk_client_gone().
 *
 *  4. VMS$VAXcluster VC (SS4(L)(d)). There is exactly ONE VMS$VAXcluster
 *     connection per PAIR of systems; the 16-byte SCA connect data it carries
 *     is the Connection Managers' version handshake (book p. 2-25, SS4(N)) --
 *     see "WHAT THIS FILE REFUSES TO INVENT" below.
 *
 *     WHICH SIDE OPENS IT IS A RACE, AND EITHER OUTCOME IS NORMAL (E67,
 *     measured on the reference join vax3-2to3-established-join-20260730):
 *     the joiner VAX3 OPENED the connection to VAX1 (t+29.825) and ACCEPTED
 *     the one VAX2 opened to it (t+30.367). So this FSM issues its own
 *     connect, and if the member's inbound one arrives first it ADOPTS that
 *     one instead of opening a second -- cnxman_join_cm_accepted(), below.
 *     SS4(L)(1)'s "the CONNECT-REQUEST is JOINER->MEMBER" describes the leg
 *     the joiner won, not a rule that the joiner must win.
 *
 *  5. MODEL + PARAMS BURST on that VC (SS4(L)(e), SS4(o) rows 1-2): cat-0x01
 *     op-0x14 then op-0x01, as ORIGINATIONS (send-msg# 1 and 2), the moment
 *     the connection is OPEN -- on the reference wire this is emitted on the
 *     accepted connection exactly as on the opened one. The peer reciprocates
 *     in kind within ~1 ms (SS4(o) row 3) and this node learns the peer's
 *     VOTES and version into that peer's CSB -- a real record arriving, never
 *     a default. A joiner that does not emit this burst is never promoted:
 *     the member's CSB for it holds no parameters, and it sits in the
 *     connectivity-wait state until the reconnect timer breaks it (E67).
 *
 *  6. CONFIG, DEFERRED (SS4(o) row 4, "this starts admission"). The rule is
 *     NOT "wait N seconds" -- the measured gap is 1.44 s in one specimen and
 *     4.4 s in another -- it is "send op 0x02 when your own disk-client
 *     discovery has finished" (SS4(o)'s UPDATE 2026-08-01). So this FSM sends
 *     it from the state the OFFLINE terminator puts it in, and a timer is
 *     nowhere in that path.
 *
 *  7. THE MEMBER-DRIVEN TAIL (SS4(o) rows 5-10): op-0x03 COMMIT and each
 *     op-0x05 rebuild transaction answered with the grounded 0x81 echo, the
 *     op-0x06 MEMBERSHIP burst answered with the opportunistic cat-0x04 ack
 *     the allowlist grounds for it, the cat-0x06 close answered with THIS
 *     node's own parameter block (echoing the request's payload there
 *     bugchecked a real VAX with INCONSTATE, SS4(p)).
 *
 *  8. HAND-OFF TO THE BARRIER at the transition open / GO. From the op-0x0a
 *     XITGO onward the 12-step barrier owns the wire and this FSM originates
 *     nothing more (FC-P3.5, vms_cnxman_barrier_fsm.h).
 *
 * THE SERVER HALF runs alongside all of it. Admission is BIDIRECTIONAL
 * (SS4(y), book p. 7-11: a joiner is admitted only if EVERY member has a
 * connection to it), so a member's inbound VMS$VAXcluster connect must be
 * accepted while the join is in flight. cnxman_join_connect_req() is that
 * policy, and it is the same object's state -- one join, two directions.
 *
 * ===========================================================================
 * WHAT THIS FILE REFUSES TO INVENT (INV-6), EACH WITH ITS CONSEQUENCE
 *
 * A. THE LOCAL CSID -- E30, FALSIFIED + REPLACED by a real-VAX capture
 *    (tests/lab/captures/op06-join-20260903.pcap, 257 op-0x06 frames,
 *    docs/cluster-integration-notes.md E30). The original premise --
 *    "match our own SCSSYSTEMID in the op-0x06 MEMBERSHIP burst" -- was
 *    WRONG: op-0x06 carries the EXISTING member re-asserting ITS OWN
 *    record (or another already-admitted member's), never the joiner's, so
 *    that match could never fire on real traffic (the old instrument's
 *    honest "not found" is what falsified it). The REAL, byte-exact
 *    mechanism: CSID = (cluster_generation << 16) | (SCSSYSTEMID & 0x3FF)
 *    -- verified against both real nodes in the capture (VAX1 sysid 1025 ->
 *    CSID 0x00010001, VAX3 sysid 1027 -> CSID 0x00010003, generation 1 in
 *    both). The generation is common to every member's CSID, so THIS FSM:
 *      - reads whatever genuine CSID a received op-0x06 carries
 *        (vms_cm_membership_coordinator_csid(), which tries the two
 *        measured byte offsets and returns "not found" rather than a
 *        guess -- see vms_cluster_codec_cm.h);
 *      - takes that CSID's own high 16 bits as the WIRE-LEARNED generation
 *        (never assumed, never hardcoded);
 *      - computes THIS node's own CSID = (generation << 16) |
 *        (own_SCSSYSTEMID & 0x3FF), own_SCSSYSTEMID being this node's real,
 *        already-loaded SYSGEN parameter (FC-P0.10) -- never a template;
 *      - calls cnxman_join_csid_learned() with the computed value, which
 *        fires cnxman_club_learn_local_csid() and moves this node NEW ->
 *        MEMBER.
 *    Until an op-0x06 carrying a shape-valid CSID has been seen, this node
 *    stays NEW: no generation, no computed CSID, no invented placeholder --
 *    the exact discipline that was already correct about the *value*, now
 *    applied to a mechanism that actually fires on real traffic.
 *
 * B. LOCKDIRWT ON THE WIRE. Book D-DLM-1 and design SS5.1 have this node
 *    advertise LOCKDIRWT = 0 honestly, but WHICH PARAMS byte carries it is
 *    plan row FC-P3.2 (lab). vms_cm_params_build() writes only grounded
 *    placements and zeroes the rest, so a LOCKDIRWT of 0 and "the field was
 *    not written" are the same bytes today -- a coincidence, NOT the field
 *    being placed. This FSM says so: it counts `lockdirwt_unpinned` on every
 *    PARAMS it sends and, if the configured LOCKDIRWT is NONZERO, counts
 *    `lockdirwt_unrepresentable` and logs, because that value genuinely
 *    cannot be advertised until the offset is pinned.
 *
 * C. THE 16-BYTE CONNECT DATA. p. 2-25 makes it the Connection Managers'
 *    version handshake and gives the peer the right to REJECT on it. Spec
 *    SS4(N) grounds where it sits and what real VAXes put there, and says
 *    outright that OVMX "cannot yet generate connect data for a role it has
 *    not observed, and it must not claim to". So this file has NO built-in
 *    value: `cnxman_join_cfg.conndata` is supplied by the glue or the field
 *    goes out an explicit zero and `conndata_omitted` counts it. WHAT OVMX
 *    should declare as its cluster-protocol version identity is a design and
 *    identity-posture decision, not an FSM's to bake in.
 *
 * D. THE VMS$VAXcluster DIRECTORY DESCRIPTOR (integration note E24). The
 *    affirmative 16-byte result a directory HIT on VMS$VAXcluster carries is
 *    spec SS4(h)(2) RE gap (c) -- undecoded. This FSM declares one through
 *    scs_fsm_sysap_set_dir_data() ONLY if the glue supplied a grounded value;
 *    otherwise it declares nothing, the directory service falls back to its
 *    honest name-echo, and `dir_descriptor_omitted` counts it.
 *
 * E. THE JOIN TARGET'S PROTOCOL/ECO LEVEL. Book p. 7-37/7-38 (correction D7)
 *    has the JOINER select whom to ask, by highest VAXcluster protocol level,
 *    then highest ECO level, then the CSB nearest the end of the CLUB's CSB
 *    queue -- and asks only once the members it has connectivity with equal
 *    the member count those CSBs advertise. Neither the protocol/ECO pair nor
 *    the advertised member count has an isolated wire offset (FC-P3.2's
 *    scope). This FSM implements the RESIDUAL rule it really can evaluate --
 *    the CSB nearest the queue tail, which is live CLUB state -- and counts
 *    `target_level_unpinned` / `member_count_ungated` so the two omissions
 *    are visible in the diagnostics rather than discovered on a real cluster.
 *    It does NOT gate the join on a count it cannot read: that would be a
 *    deadlock chosen over an honest omission.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 * This TU is PURE: no seam call, no allocation, no clock but ops->now_ms, so
 * it runs identically in both kmods, in the R1 host tests and in the rung-2
 * N-node simulator.
 */
#ifndef OVMX_VMS_CNXMAN_JOIN_FSM_H
#define OVMX_VMS_CNXMAN_JOIN_FSM_H

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_barrier_fsm.h"
#include "vms_cluster_codec_cm.h"
#include "vms_mscp_cl_fsm.h"

/* The transition ring (E69). Observability only: a NULL ring is the normal
 * wiring for every caller that does not want a transcript, and this FSM's
 * behaviour is bit-identical with and without one. */
struct cnxman_diag_ring;

/* ==========================================================================
 * 1. Constants
 * ========================================================================== */

/* The two SYSAP names this node LOOKS UP before connecting to them, and the
 * two it CONNECTS AS. All four are PUBLISHED names (*VAXcluster Principles*
 * p. 2-51 for the directory pair; spec SS4(L)/SS4(h)(2) reads the other four
 * off the wire's own 16-byte ASCII name fields). Blank-padded to
 * VMS_SCS_PROCNAME_LEN, because NUL padding is a different name to a VAX. */
extern const uint8_t cnxman_join_name_vaxcluster[VMS_SCS_PROCNAME_LEN];
extern const uint8_t cnxman_join_name_mscp_disk[VMS_SCS_PROCNAME_LEN];
extern const uint8_t cnxman_join_name_disk_cl_drvr[VMS_SCS_PROCNAME_LEN];

/*
 * How often this node NOTICES that a join step is still outstanding. An OVMX
 * DESIGN VALUE, labelled as one: no capture measures a JOINER-side timeout.
 * What it does is bounded on purpose -- it counts, it logs once, and in the
 * lookup state it RE-ISSUES the inquiry, which is the recovery p. 2-51 itself
 * names ("Periodically, the Process Poller ... connects"). It never abandons a
 * join and it never fabricates an answer: a directory inquiry that goes
 * unanswered is silence, and vms_scs_dir.h reports silence as silence.
 */
#define CNXMAN_JOIN_WATCH_MS 1000u

/* The Send Credits this node extends on each connection it opens. The
 * SCS$DIR_LOOKUP value is spec SS4(h)(2a)'s byte-exact [48:50] = 3 (the same
 * figure vms_scs_dir.h adopts and explains); the other two are this node's own
 * choice, sized to the one-command-at-a-time dialogues they carry, and are
 * labelled as OVMX values rather than presented as measured ones. */
#define CNXMAN_JOIN_DIR_CREDITS   3u
#define CNXMAN_JOIN_MSCP_CREDITS  4u
#define CNXMAN_JOIN_CM_CREDITS    4u

/* ==========================================================================
 * 2. The states
 *
 * Nine, and each is a position a real joiner was OBSERVED to occupy (spec
 * SS4(L)(a)-(e), SS4(o)'s ordered table). No step of the drive is deferred on a
 * clock: the deferral of op-0x02 is a dependency on the disk walk finishing,
 * not on a timer (SS4(o)'s own correction). The one place this machine WAITS is
 * VC_CONNECT, and it waits for CONNECTIVITY rather than for time -- the clock
 * there is p. 7-30's once-a-second retry cadence, and the bound is the target
 * CSB's own reconnect window, never a number this FSM chose (E71).
 * ========================================================================== */
enum cnxman_join_state {
	CNXMAN_JOIN_IDLE        = 0, /* CLUSTER_START not called yet          */
	/*
	 * Our OWN SCS$DIRECTORY client round: the connection and the inquiries
	 * on it are ONE state because p. 2-51 makes them one act -- the poller
	 * opens the connection when it has something to ask and closes it when
	 * nothing is outstanding, and vms_scs_dir.h implements exactly that
	 * transient round. Asking IS opening.
	 */
	CNXMAN_JOIN_DIR_ROUND   = 1,
	CNXMAN_JOIN_MSCP_CONNECT = 2,/* our MSCP$DISK connect is out          */
	/*
	 * This join has NO VMS$VAXcluster connection to its member and needs
	 * one. Three different facts put it here and all three are the same
	 * position: our connect is outstanding, our connect could not be put on
	 * the wire at all, or a connection that was up went away before this
	 * node was admitted (E71). It is NOT a failure and it is NOT a "wait a
	 * while" state invented for a clock: the once-a-second beat retries
	 * from here (p. 7-30's cadence), the member's own connect is still
	 * adopted from here (p. 7-24 REACCEPT), and what BOUNDS the wait is the
	 * target CSB's own reconnect window -- read, never kept here.
	 */
	CNXMAN_JOIN_VC_CONNECT  = 3,
	CNXMAN_JOIN_ADVERTISE   = 4, /* VC open, MODEL+PARAMS sent; the disk
				      * discovery walk is running             */
	CNXMAN_JOIN_ADMIT       = 5, /* op-0x02 sent: admission started       */
	CNXMAN_JOIN_BARRIER     = 6, /* handed off; the barrier owns the wire */
	CNXMAN_JOIN_MEMBER      = 7, /* the cluster told us our CSID (see A)  */
	CNXMAN_JOIN_FAILED      = 8, /* an honest, named refusal              */
	CNXMAN_JOIN_STATE__COUNT
};

/*
 * Why an attempt STOPPED. Recorded, logged and counted -- never swallowed.
 *
 * TERMINAL vs NOT (E71). Only a VERDICT is terminal: a peer REJECT is the
 * Connection Managers' judgement on this node's version identity (p. 2-25 /
 * D12) and a codec refusal is a defect in this node, and retrying either is a
 * loop rather than a recovery. Everything else here is a fact about
 * CONNECTIVITY, and p. 7-30 is explicit that connectivity comes and goes
 * without meaning that anybody left: those values name why the CURRENT attempt
 * stopped and the join stays alive, so `failure` can be set while `state` is
 * IDLE or VC_CONNECT. That is not a contradiction -- it is "the last attempt
 * stopped for this reason, and this node is still trying".
 */
enum cnxman_join_failure {
	CNXMAN_JOIN_FAIL_NONE       = 0,
	CNXMAN_JOIN_FAIL_NO_TARGET  = 1, /* no CSB to join through            */
	CNXMAN_JOIN_FAIL_CONNECT    = 2, /* SCS refused to open a connection  */
	CNXMAN_JOIN_FAIL_REJECTED   = 3, /* the peer REJECTED our connect --
					  * p. 2-25's version gate (D12)      */
	CNXMAN_JOIN_FAIL_PATHLOST   = 4, /* the circuit went while we joined  */
	CNXMAN_JOIN_FAIL_ABSENT     = 5, /* "NOT PRESENT HERE": the member
					  * does not run the SYSAP we need    */
	CNXMAN_JOIN_FAIL_SEND       = 6, /* a body we built could not be sent */
	CNXMAN_JOIN_FAIL_CODEC      = 7, /* the codec refused to build one    */
	/*
	 * The p. 7-30 reconnect timeout period ran out with no VMS$VAXcluster
	 * connection to the member: the CSB ladder gave that connection up
	 * (vms_cnxman_csb.c's csb_give_up()) and this join read that, rather
	 * than keeping a hope of its own. The HONEST end of a wait -- this node
	 * is not a member and does not claim to be.
	 */
	CNXMAN_JOIN_FAIL_TIMEOUT    = 8,
	CNXMAN_JOIN_FAIL__COUNT
};

/* What an offered inbound frame did. Same vocabulary as the barrier's and the
 * coordinator's, plus one value they do not need. */
enum cnxman_join_rx {
	CNXMAN_JOIN_RX_CONSUMED = 0, /* handled here (or forwarded, below)   */
	CNXMAN_JOIN_RX_HANDOFF  = 1, /* a transition frame and NO barrier is
				      * installed: the caller must route it   */
	CNXMAN_JOIN_RX_NOT_MINE = 2, /* another CM FSM owns it               */
	CNXMAN_JOIN_RX_BAD      = 3  /* it did not parse as a CM frame       */
};

const char *cnxman_join_state_name(enum cnxman_join_state s);
const char *cnxman_join_failure_name(enum cnxman_join_failure f);

/* ==========================================================================
 * 3. This node's own identity, as CONFIGURATION
 *
 * Everything this FSM asserts about ITSELF on the wire arrives here, from the
 * glue, read out of real executive state -- and every field carries a `_valid`
 * companion so an unsupplied one is an HONEST OMISSION (an explicit zero,
 * counted) and never a plausible default. There is no captured constant in
 * this struct's defaults because there are no defaults.
 * ========================================================================== */
struct cnxman_join_cfg {
	/* cat-0x01 op-0x14: this node's CPU/model string (spec SS4(j) row 1).
	 * `model_len` 0 advertises none, which is what a node whose executive
	 * could not read a model name honestly has to say. */
	uint8_t  model[VMS_CM_MODEL_MAX];
	uint8_t  model_len;
	uint8_t  model_valid;

	/* cat-0x01 op-0x01 body[88:96]: this node's OWN 8-byte version string.
	 * Spec SS4(L)(6) measured that the field is display-only and that a
	 * real VAX prints a non-"VMS" string verbatim, so OVMX advertises its
	 * own (honest-OS-identity ruling) and this file bakes in nothing. */
	uint8_t  version[VMS_CM_VERSION_LEN];
	uint8_t  version_valid;

	/* body[72:76] / body[76:80] of the node-parameter block. Spec SS4(j)
	 * records the values a real VAX carries there as OBSERVED CONSTANTS
	 * whose MEANING is unknown; OVMX therefore sends whatever the glue can
	 * justify and, with nothing supplied, an explicit zero. Copying the
	 * observed values would be a template constant asserted as this node's
	 * state -- the exact thing INV-6 forbids. */
	uint32_t param_f1;
	uint32_t param_f2;
	uint8_t  params_valid;

	/* The 16-byte SCA connect data for the VMS$VAXcluster connect
	 * (see "WHAT THIS FILE REFUSES TO INVENT", C). */
	uint8_t  conndata[VMS_SCS_PROCNAME_LEN];
	uint8_t  conndata_valid;

	/* The 16 bytes a directory HIT on our own VMS$VAXcluster name should
	 * carry (integration note E24, D above). */
	uint8_t  dir_descriptor[VMS_SCS_PROCNAME_LEN];
	uint8_t  dir_descriptor_valid;

	uint8_t  pad0[2];
};

/* ==========================================================================
 * 4. The injected ops -- this FSM's ONLY route to the world
 *
 * cnxman_ops (vms_cnxman.h) already carries the clock, the timers and the
 * %CNXMAN log. What a JOIN needs beyond that is the SCS client surface, and
 * these five are it. Production bindings are named beside each; the glue is
 * five one-line thunks and holds every layer pointer, so this file holds none.
 *
 * RETURN CONVENTION: 0 accepted, nonzero REFUSED -- the one convention
 * vms_cnxman.h states for every op injected into a pure cluster FSM, and the
 * one this TU can express (it includes no vms_internal.h and cannot name an
 * SS$_ status). The production thunks return the executive's SS$_ status
 * through cnxman_fsm_rc(); a thunk that forgets reports SS$_NORMAL (= 1) as a
 * refusal and this join dies at its first step (E67).
 * ========================================================================== */
struct cnxman_join_ops {
	/*
	 * Ask `dst` whether it hosts `name`, over the transient directory
	 * connection vms_scs_dir.h owns (production: scs_dir_inquire, with the
	 * glue's own result thunk calling cnxman_join_dir_result()). The
	 * callback is NOT threaded through here on purpose: this FSM is
	 * event-driven from ONE entry point per fact, and a function pointer
	 * crossing the seam would give it two.
	 */
	int (*dir_inquire)(void *ctx, vms_scs_sysid_t dst, const uint8_t *name);

	/*
	 * Open an SCS connection from `local_name` to `remote_name` on `dst`,
	 * extending `credits` receive buffers and carrying `conndata` (16
	 * bytes, or NULL for none). Production: scs_fsm_connect. The Con.ID
	 * SCS mints is returned; this FSM records it and matches every later
	 * event against it.
	 */
	int (*connect)(void *ctx, vms_scs_sysid_t dst,
		       const uint8_t *local_name, const uint8_t *remote_name,
		       const uint8_t *conndata, uint16_t credits,
		       vms_conid_t *out_conid);

	/*
	 * Send one application-message BODY on `conid`: 132 bytes for a
	 * VMS$VAXcluster body, 36 for an MSCP command (the two GROUNDED
	 * application-message classes, vms_scs_fsm.h SS1). Production:
	 * scs_fsm_send_msg, which spends a real credit and fills abs 56-71
	 * from the CDT -- so not one envelope byte is this FSM's.
	 */
	int (*send_msg)(void *ctx, vms_conid_t conid,
			const uint8_t *body, uint32_t len);

	/* Tear a connection down (p. 2-51: the directory connection is
	 * TRANSIENT and does not survive the round). Production:
	 * scs_fsm_disconnect. */
	int (*disconnect)(void *ctx, vms_conid_t conid);

	/*
	 * Declare the 16 bytes an affirmative directory answer about `name`
	 * carries (production: scs_fsm_sysap_set_dir_data). Called ONLY when
	 * the glue supplied a grounded descriptor -- see E24.
	 */
	int (*set_dir_data)(void *ctx, const uint8_t *name,
			    const uint8_t *data);

	/*
	 * This node's own VMS absolute time, for the MSCP SET CONTROLLER
	 * CHARACTERISTICS P.TIME field (sec 6.16; vms_mscp_cl_fsm.h makes it
	 * caller-supplied because reading a clock is a seam call and a pure TU
	 * may not make one). 0 is the specification's own "no time supplied".
	 */
	uint64_t (*time_now)(void *ctx);

	void *ctx;
};

/* ==========================================================================
 * 5. The context
 *
 * No globals (design SS3.9 rule 3). Every counter below is incremented from a
 * real dispatch of a real event; not one is displayed as cluster state.
 * ========================================================================== */

/* Which lookups have been answered affirmatively. One bit per name, so the
 * MSCP$DISK connect is literally unreachable without its HIT. */
#define CNXMAN_JOIN_L_MSCP_DISK  0x01u
#define CNXMAN_JOIN_L_VAXCLUSTER 0x02u
#define CNXMAN_JOIN_L_ALL        (CNXMAN_JOIN_L_MSCP_DISK | \
				  CNXMAN_JOIN_L_VAXCLUSTER)

/* One bit per sec 4(o) origination, for `burst_on_conn` above. */
#define CNXMAN_JOIN_B_MODEL   0x01u
#define CNXMAN_JOIN_B_PARAMS  0x02u
#define CNXMAN_JOIN_B_CONFIG  0x04u

/* How many served units this FSM will record from one walk. The walk itself is
 * unbounded (it ends at the peer's own OFFLINE terminator); this bounds only
 * how many of the peer's answers are KEPT, and an overflow is counted, never
 * silently dropped. */
#define CNXMAN_JOIN_MAX_UNITS 16u

struct cnxman_join {
	struct vms_cluster           *cl;
	const struct cnxman_ops      *ops;
	const struct cnxman_join_ops *jops;
	struct cnxman_join_cfg        cfg;

	/* The transition barrier this join hands off to (FC-P3.5). NULL is a
	 * legitimate wiring -- the glue may route transition frames itself --
	 * and then cnxman_join_rx_frame() answers CNXMAN_JOIN_RX_HANDOFF
	 * instead of forwarding. */
	struct cnxman_barrier        *barrier;

	/*
	 * The E69 transition ring, or NULL. RECORDING ONLY -- nothing this FSM
	 * does is conditional on it, no handler consults it, and every write to
	 * it is a bounded store into a caller-owned buffer. Installed after
	 * cnxman_join_init() (which zeroes this struct) by
	 * cnxman_join_set_diag(); production installs the one the connection
	 * manager allocates beside itself, host tests install a stack ring, and
	 * a build that wants no diagnostics installs none.
	 */
	struct cnxman_diag_ring      *diag;

	uint8_t  state;              /* enum cnxman_join_state               */
	uint8_t  failure;            /* enum cnxman_join_failure             */
	uint8_t  lookups_hit;        /* names the member really HOSTS        */
	uint8_t  lookups_answered;   /* names it has ANSWERED about at all --
				      * a HIT and a "NOT PRESENT HERE" are two
				      * different facts and both are answers  */
	uint8_t  tr_class;           /* our own current class, for the 0x81
				      * echo's body[17] (spec SS4(r))         */
	uint8_t  pad_a[3];

	/* ---- the member this node is joining THROUGH (book p. 7-37/38) ---- */
	vms_scs_sysid_t target_sysid;
	uint8_t  target_valid;
	uint8_t  pad0[3];
	int32_t  target_csb;         /* CLUB slot, -1 = none                 */

	/*
	 * The two connections this join OWNS, by the Con.ID SCS minted. The
	 * directory connection is deliberately absent: it belongs to the
	 * transient round vms_scs_dir.h opens and closes on p. 2-51's own
	 * rule, and a second owner of that lifecycle is how a "transient"
	 * connection stops being one.
	 */
	vms_conid_t mscp_conid;
	vms_conid_t cm_conid;
	uint8_t  mscp_open, cm_open;

	/*
	 * WHICH of the sec 4(o) originations this node has really transmitted
	 * ON THE CONNECTION IT HOLDS NOW (CNXMAN_JOIN_B_*, below). Cleared the
	 * instant `cm_conid` changes, because a message that went out down a
	 * connection that has since gone is a message the member's NEW
	 * connection never carried. The three `*_sent` counters below are
	 * LIFETIME tallies and cannot answer that question -- reading them as
	 * "already advertised" is how a re-offer silently stops happening after
	 * a reconnect (E71).
	 */
	uint8_t  burst_on_conn;
	uint8_t  pad1[1];

	/* ---- the disk-client discovery walk (FC-P3.4) ---- */
	struct vms_mscp_cl_fsm  mscp;
	struct vms_mscp_cl_unit units[CNXMAN_JOIN_MAX_UNITS];
	uint32_t units_found;        /* units the PEER really reported       */
	uint32_t units_dropped;      /* ... beyond the table above           */
	uint8_t  mscp_walk_done;
	uint8_t  pad2[3];

	/* ---- what this node did, all counted from real dispatches ---- */
	uint32_t joins_started;
	uint32_t lookups_sent;
	uint32_t lookups_hits;
	uint32_t lookups_misses;     /* the wire's literal "NOT PRESENT HERE" */
	uint32_t lookups_reissued;   /* the p. 2-51 poll repeat               */
	uint32_t mscp_absent;        /* the member serves no disks: a real
					configuration, not a failure          */
	/*
	 * The member REFUSED (E68) or DROPPED this node's disk-client
	 * connection. Neither is a join failure -- see "THE DISK-CLIENT
	 * CONNECTION IS NOT A MEMBERSHIP PREREQUISITE" in the .c -- and both
	 * are counted so an operator can tell "we never asked" from "we asked
	 * and were turned away".
	 */
	uint32_t mscp_rejected;
	uint32_t mscp_lost;
	/*
	 * ... and the third way that connection can fail to exist: this node's
	 * own SCS refused to put the disk-client connect on the wire at all
	 * (E71 -- it happens: the CDT, the circuit or the port can all say no).
	 * Same verdict as the other two, for the same reason: MSCP$DISK is not
	 * a membership prerequisite, so the join goes straight on to step 4.
	 */
	uint32_t mscp_connect_refused;

	/* ---- staying alive through a transient (E71) ---- */
	/*
	 * Our VMS$VAXcluster connect could not be put on the wire. NOT a join
	 * failure: nothing was asserted to anyone, and the member's own connect
	 * is still coming. Counted, and retried on the beat.
	 */
	uint32_t cm_connect_refused;
	/* A VMS$VAXcluster connection this join HELD went away (p. 7-30: do not
	 * presume the member left). */
	uint32_t cm_lost;
	/* Beats on which VC_CONNECT re-issued the connect (p. 7-30's cadence). */
	uint32_t cm_reattempts;
	/* Beats on which the Con.ID was taken from the TARGET CSB instead --
	 * the executive's reconnect apparatus had opened one this FSM had no
	 * other way of learning about. */
	uint32_t cm_resynced;
	/* The reconnect timeout period ran out (FAIL_TIMEOUT): the attempt was
	 * released and this node went back to waiting for a cluster. */
	uint32_t connect_windows_expired;
	/* Attempts that never started because no CSB was joinable at that
	 * instant -- VMS's "waiting to form or join", counted rather than
	 * turned into a terminal failure. */
	uint32_t starts_deferred;
	uint32_t model_sent;
	uint32_t params_sent;
	uint32_t config_sent;
	/*
	 * Watchdog passes in ADVERTISE/ADMIT that RE-OFFERED at least one
	 * origination of the sec 4(o) burst because SCS had refused it and it
	 * therefore never left this node -- p. 2-51's "the poller REPEATS",
	 * the same recovery `lookups_reissued` counts for the directory round
	 * (E70). ATTEMPTS, not successes: whether a re-offer got through is
	 * what the three counters above say. A message that was really
	 * transmitted is never re-offered, so this can never double-send.
	 */
	uint32_t burst_reoffers;
	uint32_t echoes_sent;        /* 0x81 answers to op-0x03 / op-0x05     */
	uint32_t acks_sent;          /* cat-0x04 answers to op-0x06           */
	uint32_t closes_answered;    /* cat-0x06 close, own parameter block   */
	uint32_t mscp_cmds_sent;
	uint32_t mscp_ends;
	uint32_t peer_adverts;       /* the member's own 0x14/0x01/0x02       */
	uint32_t peer_acks;          /* cat-0x04 acks the member sent us      */
	uint32_t inbound_accepted;   /* members' connects (total connectivity)*/
	uint32_t inbound_refused;    /* ... refused, with a reason            */
	uint32_t cm_adopted;         /* the member won the connect race and
				      * THIS is the one connection to it (E67)*/
	uint32_t cm_already_held;    /* an accepted VMS$VAXcluster connection
				      * arrived while we already had one to the
				      * same member: NOT adopted, counted      */
	/*
	 * Accepted VMS$VAXcluster connections from a member that is NOT the one
	 * this join runs its dialogue with. The Rule of Total Connectivity
	 * requires this node to TAKE them (cnxman_join_connect_req does), and
	 * the reference joiner also runs the op-0x14/op-0x01 identity exchange
	 * on every one of them -- which this single-target FSM does not yet do.
	 * Counted rather than guessed at, so the gap is visible in the
	 * diagnostics instead of on a real cluster (plan follow-up, not this
	 * FSM's to invent).
	 */
	uint32_t cm_other_member;
	uint32_t handoffs;           /* transition frames given to the barrier*/
	uint32_t slow_steps;         /* the watchdog fired; NEVER an abort    */
	uint32_t send_failures;
	uint32_t codec_failures;
	uint32_t ignored_events;     /* [state][event] with no edge: COUNTED  */

	/* ---- the honest omissions, each visible in the diagnostics ---- */
	uint32_t membership_records; /* op-0x06 bursts received               */
	uint32_t csid_unpinned;      /* ... from which no coordinator CSID
				      * could be read (E30): no shape-valid
				      * CSID at either measured offset yet    */
	uint8_t  lockdirwt_unrepresentable; /* configured nonzero, no offset  */
	uint8_t  pad3[3];
	uint32_t lockdirwt_unpinned;
	uint32_t conndata_omitted;
	uint32_t dir_descriptor_omitted;
	uint32_t model_omitted;
	uint32_t version_omitted;
	uint32_t node_params_omitted;
	uint32_t target_level_unpinned;
	uint32_t member_count_ungated;

	/*
	 * The ONE scratch buffer every built body goes through. In the
	 * context, not on the stack: this code runs on a VAX kernel stack.
	 * Sized for the CM body (132); the 36-byte MSCP command body is built
	 * into `mscp_frame` below and sent from its abs-72 slice.
	 */
	uint8_t scratch[VMS_CM_BODY_LEN];

	/*
	 * The MSCP command builder writes a whole 108-byte FRAME (FC-P3.4's
	 * seam takes a `struct vms_mscp_link` for abs [0,72)). This FSM sends
	 * BODY-LEVEL, so it passes an ALL-ZERO link and transmits only
	 * frame[72:108]: abs 56-71 is then filled by SCS from the real CDT and
	 * abs 0-55 by the port from the real circuit (design SS3.2.4 ruling
	 * E1). Not one byte of the zero prefix ever reaches the wire.
	 */
	uint8_t mscp_frame[VMS_MSCP_CMD_FRAME_LEN];
};

/* ==========================================================================
 * 6. Lifecycle
 * ========================================================================== */

/* Bind the FSM to a node. Sends nothing, arms nothing, connects nothing. */
void cnxman_join_init(struct cnxman_join *j, struct vms_cluster *cl,
		      const struct cnxman_ops *ops,
		      const struct cnxman_join_ops *jops);

/* This node's own identity, as read from real executive state by the glue.
 * `cfg` NULL clears it -- and then every field this FSM would have asserted
 * becomes an explicit, counted omission. */
void cnxman_join_set_cfg(struct cnxman_join *j,
			 const struct cnxman_join_cfg *cfg);

/* Install (or, with NULL, detach) the transition barrier this join hands off
 * to at the op-0x0a GO. */
void cnxman_join_set_barrier(struct cnxman_join *j, struct cnxman_barrier *b);

/*
 * Install (or, with NULL, detach) the E69 transition ring this join records
 * into. Call AFTER cnxman_join_init(), which zeroes the context.
 *
 * OBSERVABILITY ONLY (INV-6). Installing a ring changes no state, arms no
 * timer, sends nothing and alters no decision this FSM makes; removing one
 * loses history and nothing else. The ring's own serialization is the
 * CALLER's -- in production every join event is already dispatched from the
 * one cluster fork context under the fork mutex, and the diagnostics ioctl
 * reads under that same mutex, so this adds no lock and no ordering.
 */
void cnxman_join_set_diag(struct cnxman_join *j, struct cnxman_diag_ring *r);

/* ==========================================================================
 * 7. Events -- one entry point per FACT
 * ========================================================================== */

/*
 * CLUSTER_START: begin the join. Selects the member to join through from the
 * real CLUB (see E above), declares this node's directory descriptor if one
 * was supplied, and opens the SCS$DIRECTORY connection.
 *
 * Returns 0 when a drive really started, non-zero when it did not -- and the
 * named reason is in `failure` either way. A start that did not happen leaves
 * this FSM in IDLE (nothing was asserted to anybody, so there is nothing to
 * fail), which is what lets the caller's once-a-second beat ask again: VMS's
 * own "waiting to form or join an OpenVMS Cluster" is a REPEATED question, not
 * one attempt with a terminal answer (E71).
 */
int cnxman_join_start(struct cnxman_join *j);

/* One of THIS FSM's connections reached OPEN. */
void cnxman_join_opened(struct cnxman_join *j, vms_conid_t conid);

/* One of them closed. `reason` is the glue's SS$_ status; SS$_PATHLOST and a
 * REJECT are distinguished by cnxman_join_rejected() below. */
void cnxman_join_closed(struct cnxman_join *j, vms_conid_t conid,
			uint32_t reason);

/*
 * The peer REJECTED a connect we made.
 *
 * On the `VMS$VAXcluster` connection that is book p. 2-25's version gate
 * (correction D12): the Connection Managers identify themselves to each other
 * in the 16-byte connect data and reject a version they do not approve of, so
 * the join fails with CNXMAN_JOIN_FAIL_REJECTED and one %CNXMAN line naming
 * the identity gate -- a silent retry against a peer that has judged our
 * version is a loop, not a recovery.
 *
 * On the `MSCP$DISK` disk-client connection it is NOT that verdict and NOT a
 * join failure (E68): that connect carries no connect data at all, so there is
 * no version for the peer to have judged. See "THE DISK-CLIENT CONNECTION IS
 * NOT A MEMBERSHIP PREREQUISITE" in the .c for the full grounding.
 */
void cnxman_join_rejected(struct cnxman_join *j, vms_conid_t conid,
			  uint32_t reason);

/*
 * A directory inquiry was ANSWERED. `present` nonzero is a HIT, zero is the
 * wire's literal "NOT PRESENT HERE". An inquiry that was never answered does
 * not arrive here at all (vms_scs_dir.h reports silence as silence); the
 * watchdog re-issues it.
 */
void cnxman_join_dir_result(struct cnxman_join *j, vms_scs_sysid_t from,
			    const uint8_t *name, int present);

/*
 * Does THIS join already hold the `MSCP$DISK` (disk-client) connection to
 * `dst`? Nonzero only when the join really opened one -- a live `mscp_conid`
 * against the member it joined THROUGH -- never from the target selection
 * alone. The MSCP class driver's own sweep asks this so that OVMX presents
 * exactly ONE `VMS$DISK_CL_DRVR` -> `MSCP$DISK` connection per member, which
 * is what every reference joiner does (vms_mscp_cl_conn_fsm.h, Rule 2's
 * corollary). A read of real FSM state; it asserts nothing.
 */
int cnxman_join_holds_disk_client(const struct cnxman_join *j,
				  vms_scs_sysid_t dst);

/* One MSCP END message arrived on the MSCP$DISK connection: the whole frame as
 * the port delivered it (receive stays frame-based, design SS3.2.4). */
void cnxman_join_rx_mscp(struct cnxman_join *j, vms_conid_t conid,
			 const uint8_t *frame, uint32_t len);

/*
 * One inbound `VMS$VAXcluster` frame. Classified through the CM codec, mapped
 * to a shared enum cnxman_event and dispatched through the [state][event]
 * table. `from_csid` is the sender as the connection manager identified it and
 * `from_valid` is 0 when it could not -- in which case no identity is
 * recorded (a zero CSID is never "node zero").
 *
 * Transition frames (op 0x08/0x09/0x0a/0x0b/0x0c/0x0f and the cat-0x02
 * rebuild records) belong to the barrier: with a barrier installed they are
 * FORWARDED to it and the answer is CONSUMED; with none they come back as
 * CNXMAN_JOIN_RX_HANDOFF for the caller to route. Those are the only two
 * shapes -- a frame is never both delivered and handed back.
 */
enum cnxman_join_rx cnxman_join_rx_frame(struct cnxman_join *j,
					 const uint8_t *frame, uint32_t len,
					 vms_csid_t from_csid, int from_valid);

/*
 * THE SERVER HALF (spec SS4(y), book p. 7-11). A member is opening its own
 * `VMS$VAXcluster` connection to us; total connectivity requires that we take
 * it. Returns 0 to accept. `conndata` is the peer's 16-byte version identity,
 * recorded and counted -- never acted on, because every node we have ever
 * observed runs one VMS version and a policy built on one data point would be
 * invented (spec SS4(N)).
 */
int cnxman_join_connect_req(struct cnxman_join *j, vms_scs_sysid_t peer,
			    vms_conid_t peer_conid,
			    const uint8_t *conndata, uint32_t conndata_len);

/*
 * A member OPENED the `VMS$VAXcluster` connection to this node and SCS has it
 * OPEN (E67). `peer` is the member's SCSSYSTEMID and `conid` the LOCAL Con.ID
 * SCS minted for the accepted connection -- both real, from the glue's own
 * accept path, never inferred here.
 *
 * There is one such connection per pair (design SS3.4; measured on the
 * reference join), so this join ADOPTS it as its own when it is with the
 * member this join runs its dialogue with and this join does not already hold
 * one; then the MODEL+PARAMS burst goes out on it exactly as it would on a
 * connection this node had opened -- immediately if the drive is already past
 * the point where it would have opened one, otherwise when the drive gets
 * there (which keeps step 2's lookup-before-connect and step 3's disk walk in
 * their measured order). Every other case is COUNTED and changes nothing: an
 * accepted connection from another member is `cm_other_member`, one arriving
 * when we already have ours is `cm_already_held`.
 */
void cnxman_join_cm_accepted(struct cnxman_join *j, vms_scs_sysid_t peer,
			     vms_conid_t conid);

/*
 * The cluster told this node its CSID. See "WHAT THIS FILE REFUSES TO INVENT"
 * A: the table cell is real and is exercised at R1, and the ONLY thing that
 * can fire it on a wire is a membership-record parse the op-0x06 layout does
 * not yet permit. Nothing in this file calls it.
 */
void cnxman_join_csid_learned(struct cnxman_join *j, vms_csid_t csid);

/*
 * The join watchdog (CNXMAN_TIMER_JOIN). INSTRUMENT-AND-REPEAT, never abandon:
 * it counts a slow step, logs the first one, and in the lookup state re-issues
 * the outstanding inquiry -- p. 2-51's own recovery. It expires nothing.
 */
void cnxman_join_timer(struct cnxman_join *j);

/* ==========================================================================
 * 8. Readback
 * ========================================================================== */

/* Has this join reached the point where the barrier owns the wire? */
int cnxman_join_handed_off(const struct cnxman_join *j);

/* The units the peer's own GET UNIT STATUS answers enumerated. Returns the
 * count and, with `out` non-NULL, copies up to `cap` of them. Every field is
 * the peer's own answer; nothing here is synthesised (INV-6). */
uint32_t cnxman_join_units(const struct cnxman_join *j,
			   struct vms_mscp_cl_unit *out, uint32_t cap);

#endif /* OVMX_VMS_CNXMAN_JOIN_FSM_H */
