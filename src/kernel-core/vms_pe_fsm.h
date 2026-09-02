/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_pe_fsm.h - the NISCA CHANNEL state machine (FC-P0.8).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.4 ("per remote system a
 * PB with the set of verified channels (local netif x remote MAC)"), SS3.9 (pure
 * FSM, table-driven [state][event], one small handler per edge, injected ops and
 * injected clock). The layer's interface -- pe_ops, pe_event, pe_timer, the
 * snapshot views -- is vms_pe.h and is NOT redefined here.
 *
 * Wire grounding, and ONLY the wire spec (docs/cluster-protocol-spec.md):
 *   SS4(a)     the shared discovery header both HELLO and SOLICIT carry
 *   SS4(a).0   a directed HELLO addresses the peer's cluster-LOGICAL address at
 *              abs 16, NOT the hardware MAC at abs 0. Mirroring abs 0 into
 *              abs 16 makes the peer SILENTLY DROP every reply, and it took a
 *              third node with a non-DECnet HW MAC to see it.
 *   SS4(a).1   the b2/b3/b4 channel-verify ladder and its response rule
 *   SS4(b)     the HELLO tail: incarnation at abs 92, poller sweep at abs 128,
 *              the sender's REAL hardware MAC at abs 120
 *   SS4(c)     SOLICIT
 *   SS4(i).B   what the incarnation counter MEANS, and who advertises it
 *   SS4(k)     the padded HELLO: NISCA channel packet-size verification
 *   SS4(M)     how long a live peer may say nothing (the listen timeout)
 *   SS4(q)     the steady-state HELLO cadence a member must keep meeting
 *   SS4(O.30)  the port-level clean-leave "last gasp"
 *
 * NISCA/HELLO is NOT in the *VAXcluster Principles* transcript (ch. 3 is absent
 * from the host copy), so NOTHING here is grounded in the book. Where the wire
 * spec describes an observation but not a rule, the code says INFERRED and says
 * why -- the same discipline vms_cnxman_csb.c's table uses.
 *
 * ---------------------------------------------------------------------------
 * WHAT A CHANNEL IS, AND WHY IT HAS A LADDER
 *
 * A CHANNEL is one (local interface, remote station) pair. Before the port will
 * carry a virtual circuit over a channel, the two ends VERIFY it: they exchange
 * a three-step handshake carried in the abs-30 word of a DIRECTED HELLO, and
 * then they keep re-verifying it for as long as the channel lives.
 *
 *   b2  INIT     the established member's FIRST directed contact. GROUNDED: the
 *                joiner NEVER originates b2 (0 of 213 joiner directed HELLOs in
 *                the clean formation capture).
 *   b3  REQUEST  "verify this channel". Any node may originate one, as a plain
 *                directed HELLO or as a SS4(k) size-padded one.
 *   b4  CONFIRM  the ack, sent within ~0.2 ms of a received b3. TERMINAL --
 *                there is no b5.
 *
 * The response rule (GROUNDED byte-exact across two independent fresh
 * formations): on receiving word X reply X+1, saturating at b4. After the
 * bootstrap the two ends oscillate b3<->b4 indefinitely, each re-initiating on
 * its poller-sweep timer ~1-10 s apart, the initiator role alternating.
 *
 * OVMX HOLDS THE b2 BACK, DELIBERATELY. There is no edge in this table that
 * emits b2: SS4(a).1 grounds b2 as the established member's marker and grounds
 * that a joiner emits none, and the campaign's own hardest-won lesson is that
 * OVMX originating an unsolicited frame is what stalled the peer (removing an
 * unsolicited op-01 origination is what finally ran the whole admission chain).
 * OVMX initiates a verify the way SS4(a).1 says any node may: with b3. Two OVMX
 * nodes therefore bootstrap each other b3 -> b4 with no b2 anywhere, which is
 * what the R2 simulator and the R4 two-node executive harness need.
 *
 * THE LADDER STATE IS OUR OWN PROGRESS, NOT THE PEER'S. VMS_PE_CH_B4 means "a
 * b4 CONFIRM arrived FOR A b3 WE SENT". Answering somebody else's b3 with a b4
 * proves their->our direction only, so it does not advance our ladder; a b4 that
 * arrives when we have no outstanding b3 confirms nothing and is counted, not
 * believed (INV-6: a verified channel is a claim, and an unmatched ack does not
 * support it).
 *
 * ---------------------------------------------------------------------------
 * THE INCARNATION FIELD -- READ SS4(i).B BEFORE TOUCHING abs 92
 *
 * abs 92 of a DIRECTED HELLO is not "the sender's generation". GROUNDED,
 * SS4(i).B, byte-exact across six specimens:
 *
 *   - it is the incarnation number the SENDER attributes to the RECEIVER;
 *   - the established member advertises it, counting how many times THAT PEER
 *     has re-formed its channel (1, then 2, then 3, ...);
 *   - "The joiner's own directed HELLO always carries [78:80] = 0x0001";
 *   - the joiner must ECHO the member's advertised value into its 0x41 START
 *     [22:24], and a member holding a residual CSB advertises 2 while a joiner
 *     that hard-codes 1 STALLS at config-round 0 (that was vms-691).
 *
 * So this FSM does two separate things with it, and conflating them is the bug
 * SS4(i).B exists to prevent:
 *
 *   1. It RECORDS what each peer advertises (peer_incarnation, with a validity
 *      flag). That value is real wire data and is what FC-P1.2's START must
 *      echo. This FSM never invents it and never defaults it.
 *   2. It STAMPS 1 into every directed HELLO OVMX sends, and 0 into every
 *      multicast one. Both are GROUNDED constants of the sender role, not
 *      copies of anything received.
 *
 * A CHANGE in a peer's advertisement is the plan item's "the remote re-formed
 * this channel": the peer has decided the previous generation is gone, so the
 * b4 this node reached belongs to a channel that no longer exists. The channel
 * is RESET -- ladder back to SEEN, size verification discarded, upper layers
 * told -- and re-verified from scratch. (The plan's shorthand for this is "the
 * remote rebooted"; SS4(i).B's grounded reading is the one implemented, because
 * a reboot is only one of the ways a peer re-forms a channel.)
 *
 * ---------------------------------------------------------------------------
 * PACKET-SIZE VERIFICATION (SS4(k)) -- WHAT IS GROUNDED AND WHAT IS OBSERVED
 *
 * A padded HELLO is a genuine 134-byte directed HELLO followed by a run of
 * zeros, carrying b3: it is a size-flavoured verify REQUEST, not an opcode
 * (the campaign spent a lab week calling it "op-0xb3 block transfer"). Two
 * separate grounded facts drive the code:
 *
 *   - SS4(a).1: "the padded SS4(k) size-verify HELLOs are simply b3 REQUESTs
 *     (each acked by a plain b4)". So OUR padded probe is confirmed by a PLAIN
 *     b4 -- that, and only that, sets verified_pktsz.
 *   - SS4(k): in the successful formation there is EXACTLY ONE padded HELLO per
 *     direction; in the stalled one the joiner sends zero and the member
 *     retransmits forever. So a received padded HELLO must be RECIPROCATED with
 *     our own padded HELLO on the reverse channel.
 *
 * The size ladder 1500 -> 1069 -> 853 -> 745 at ~4 frames per size, 6.010 s
 * apart, is what the member was OBSERVED doing when unacked (SS4(k), 24/24
 * inter-frame gaps). The spec is explicit that "the precise PEDRIVER size-search
 * rule is not derivable from passive capture", so this file uses the observed
 * SIZES as a table with the cite and does NOT reconstruct a search rule
 * (CLAUDE.md Rule 8: never recompute an unpublished algorithm). Past the last
 * rung it stops probing and counts, rather than inventing a next step.
 *
 * ---------------------------------------------------------------------------
 * INCLUDES: kernel-core headers and the codec only -- design SS3.9's table
 * permits a `_fsm.c` exactly that, and tools/ci/cluster_core_includes_gate.sh
 * RULE 4 additionally forbids this TU from touching any seam primitive. Time,
 * transmission, timers and logging arrive through `struct pe_ops` and nowhere
 * else, which is what lets a 20-second listen timeout be exercised in
 * microseconds at rung 1.
 */
#ifndef OVMX_VMS_PE_FSM_H
#define OVMX_VMS_PE_FSM_H

#include "vms_cluster.h"
#include "vms_cluster_codec.h"
#include "vms_cluster_codec_hello.h"
#include "vms_cluster_codec_vc.h"   /* FC-P1.1: START/STACK/ACK + 0x48 + stamp */
#include "vms_cluster_snapshot.h"
#include "vms_pe.h"

/* ==========================================================================
 * 1. The abs-30 per-frame words (spec SS4(a), SS4(a).1, SS4(O.30))
 *
 * The state lives in the LOW byte; the high byte is 0x00. These are OBSERVED
 * VALUES with OVMX working labels (SS4(a).1's own clean-room note), not a
 * documented opcode enum.
 * ========================================================================== */
#define PE_PFW_MULTICAST  0xa0u  /* periodic multicast HELLO (SS4(a))          */
#define PE_PFW_LAST_GASP  0xb1u  /* clean-leave departure marker (SS4(O.30))   */
#define PE_PFW_VERIFY_B2  0xb2u  /* channel INIT -- member's first contact     */
#define PE_PFW_VERIFY_B3  0xb3u  /* channel-verify REQUEST                     */
#define PE_PFW_VERIFY_B4  0xb4u  /* channel-verify CONFIRM (terminal)          */
#define PE_PFW_SOLICIT    0xb6u  /* boot-time SOLICIT (SS4(a))                 */

/* ==========================================================================
 * 2. Timings
 *
 * Each is either a MEASURED wire figure with its cite, or an OVMX choice
 * LABELLED as one (Rule 8: OVMX never presents its own number as a VMS
 * constant). Every one of them is overridable through struct pe_identity, so a
 * SYSGEN value always wins over the default.
 * ========================================================================== */

/*
 * The HELLO cadence. SS4(q) measures the steady-state cadence of a real member
 * at ~2.3 s and names keeping it an ongoing membership obligation; SS4(M)
 * measures the longest silence any healthy node showed in 747 s of captured
 * wire at 3.153 s. 2000 ms sits under both, so an OVMX node is never the
 * quietest thing on the LAN. OVMX's choice, not a published VMS parameter.
 */
#define PE_HELLO_INTERVAL_DEFAULT_MS 2000u

/*
 * The listen timeout: how long a channel may hear nothing before this node
 * declares it gone. SS4(M) grounds the two populations -- healthy silence never
 * exceeded 3.153 s, a real departure showed 395.955 s, and they do not overlap.
 * The default is RECNXINTERVAL seconds (20 in the lab), 6.3x the longest healthy
 * silence and 20x under the observed departure. Again OVMX's choice: SS4(M) is
 * explicit that RECNXINTERVAL governs removal AFTER a circuit breaks, not the
 * timer that breaks it, and no published document names this one.
 */
#define PE_LISTEN_TIMEOUT_DEFAULT_MS 20000u

/*
 * The size-probe retransmit interval. GROUNDED: 6.010 s +- 0.15 across 24/24
 * inter-frame gaps of the member's unacked padded-HELLO ladder (SS4(k)), a
 * distinct timer from RECNXINTERVAL.
 */
#define PE_PROBE_RETRANSMIT_MS 6010u

/* GROUNDED as observed: ~4 frames per size before the ladder steps down. */
#define PE_PROBE_TRIES_PER_SIZE 4u

/*
 * The observed size ladder (SS4(k)), largest first. 1500 == NISCS_MAX_PKTSZ
 * 1498 + 2, byte-exact against the SYSGEN tunable. These are the sizes a real
 * member was seen to probe, USED AS A TABLE -- the search rule that produced
 * them is not published and is not reconstructed here.
 */
#define PE_PROBE_LADDER_N 4u
extern const uint16_t pe_probe_ladder[PE_PROBE_LADDER_N];

/* SS4(b) abs 128: 31 decimal, byte-exact against SDA SHOW PORTS "Poller Sweep
 * 31" on PEA0. 0 on a multicast HELLO, 0x001f on a directed one. */
#define PE_POLLER_SWEEP_DIRECTED 0x001fu

/*
 * SS4(i).B: "The joiner's own directed HELLO always carries [78:80] = 0x0001".
 * Not a copy of anything received -- a constant of the sender role.
 */
#define PE_OWN_DIRECTED_INCARNATION 1u

/* How many (interface, station) pairs one port tracks. VMS_CLUB_MAX_CSB is the
 * cluster scale the book contemplates ("30, 40, or even 96 systems"), and P0
 * binds exactly one interface, so one channel per system is the bound. A full
 * table means this FSM never allocates: it has no allocator to reach for. */
#define PE_MAX_CHANNELS VMS_CLUB_MAX_CSB

/* ==========================================================================
 * 3. What a dispatch asks the caller to do
 *
 * The channel FSM TRANSMITS on its own (ops->send is part of pe_ops -- the port
 * owns the wire), but a change in a channel's usability is a fact the layers
 * above must act on, so it comes back as an action rather than as a callback
 * this pure TU would have to hold.
 * ========================================================================== */
enum pe_channel_action {
	PE_CH_ACT_NONE = 0,
	PE_CH_ACT_VERIFIED,   /* b4 reached: the channel may carry a circuit    */
	PE_CH_ACT_RESET,      /* the peer re-formed it: the old generation is void */
	PE_CH_ACT_LOST,       /* listen timeout or link down: no connectivity   */
	PE_CH_ACT_DEPARTED,   /* SS4(O.30) last gasp: the peer ANNOUNCED it left */
	PE_CH_ACT__COUNT
};

/* ==========================================================================
 * 3b. THE VIRTUAL CIRCUIT (FC-P1.2) -- read this before touching a counter
 *
 * A CHANNEL is a (local interface, remote station) pair; a VIRTUAL CIRCUIT is
 * the reliable, SEQUENCED conversation with the remote SYSTEM that rides one.
 * The channel proves connectivity; the circuit carries meaning.
 *
 * ---------------------------------------------------------------------------
 * FORMATION -- START / STACK / ACK, and the ONE field that is the gate
 *
 * *VAXcluster Principles* p. 2-12/2-14 gives the dialogue and its acceptable-
 * response table, and this file implements exactly that table:
 *
 *   START SENT      + START -> send STACK, go START RECEIVED (both ends
 *                              started at once: the asymmetric-timing case)
 *                   + STACK -> OPEN, send ACK
 *   START RECEIVED  + ACK   -> OPEN
 *                   + STACK -> OPEN, send ACK
 *                   + START -> re-send STACK (stay)
 *                   + any circuit-requiring packet -> IMPLIED ACK, OPEN
 *                              (p. 2-16) and then process that packet
 *   OPEN            + ACK   -> discarded (p. 2-12: "each port driver simply
 *                              discards the ACK it receives")
 *
 * VMS_PE_VC_STACK_SENT is the book's START RECEIVED under the name the
 * snapshot ABI already froze (vms_cluster_snapshot.h): the state reached by
 * SENDING a STACK.
 *
 * THE INCARNATION ECHO IS THE JOIN GATE, and it is READ, never chosen. Spec
 * §4(i).B, GROUNDED byte-exact across six specimens: the member advertises,
 * in its DIRECTED HELLO at abs 92, the incarnation number IT attributes to
 * US -- 1 for a first contact, 2 for the second, 3 for the third -- and the
 * joiner must stamp exactly that number into its 0x41 START at abs 36. A
 * joiner that hard-codes 1 against a member advertising 2 STALLS at config
 * round 0 forever; that was vms-691, and it cost the campaign days.
 *
 * So this FSM takes the echo from `pe_channel.peer_incarnation`, which
 * FC-P0.8 filled from a real received frame and flagged valid, and if the
 * flag is clear it forms NO CIRCUIT AT ALL and counts it. There is no
 * default, no 1, and no "the usual value" anywhere in this file (INV-6).
 *
 * ---------------------------------------------------------------------------
 * SEQUENCING -- the mechanism, and the one failure mode that must not exist
 *
 * Spec §4(h)(4), GROUNDED across the whole formation phase: "a node holds
 * send_seq (its own next number) and recv_seq (highest peer send_seq seen); a
 * sequenced message stamps send_seq (+mirror) and recv_ack = recv_seq, then
 * increments send_seq; a 0x48 credit-return stamps [18:20] = recv_seq with
 * send_seq = 0 (no advance)."  Four consequences this file implements:
 *
 *   1. ONE send_seq PER CIRCUIT, shared by every connection multiplexed on
 *      it and CONTIGUOUS. p. 2-30/2-31: the ordered unit is the port pair,
 *      not the connection, and spec §4(O.14) measured a real VAX's circuit
 *      running 4,5,6,…,22 with no gaps across all its connections. A hole is
 *      not a local error: the peer breaks the circuit on it.
 *   2. RETRANSMIT REUSES THE SEQUENCE. Spec §4(L): a message "consumes the
 *      channel send_seq exactly once, and retransmissions REUSE that same
 *      send_seq (a retransmit is not a new message -- advancing it per
 *      retransmit desynchronizes the peer)". The retransmitted frame is
 *      re-stamped with a FRESH recv_ack, and re-marked 0x4b/0x5b -> 0x7b,
 *      which is the wire's own retransmit marking (spec §4(h), §4(O.19)).
 *   3. recv_ack IS CUMULATIVE and IS THE PEER'S RELEASE SIGNAL: every frame
 *      the peer sends carries the highest contiguous sequence it has taken
 *      from us, and everything at or below it leaves the unacked ring.
 *   4. A GAP BREAKS THE CIRCUIT. p. 2-31: "if either the guarantee of message
 *      delivery or the guarantee of message sequentiality cannot be
 *      satisfied, the virtual circuit between the ports involved will be
 *      explicitly broken … then every connection supported by this virtual
 *      circuit is also broken." Spec §4(h)(4a) measured what that detector
 *      costs on real wire: over 321,599 sequenced messages in 47 captures,
 *      ZERO gaps came from a VAX. A duplicate (a sequence at or behind
 *      recv_seq -- 506 of them) is NOT a gap and must never break anything,
 *      and the FIRST sequenced message on a circuit ANCHORS the counter
 *      rather than being scored.
 *
 * ***  THE INVARIANT THIS ITEM EXISTS FOR: recv_ack NEVER FREEZES.  ***
 *
 * A frozen recv_ack is the campaign's most expensive failure: a circuit that
 * is UP, whose peer keeps sending, and whose acknowledgement stops advancing.
 * Everything the peer sends afterwards rides behind the frozen point and is
 * never delivered -- spec §4(O.14)/§4(O.15)/§4(O.19) traced three separate
 * stalls to exactly that shape, with a real member's recv_seq stuck at 10, 19
 * and 20 while the joiner's stream ran on contiguously past it.
 *
 * Two structural rules keep this file on the right side of it:
 *
 *   (a) ACKNOWLEDGEMENT IS A TRANSPORT FACT, NOT AN APPLICATION ONE. The ack
 *       is emitted from the receive path the moment the sequence is scored --
 *       BEFORE any delivery upward, and with no `if (upper)`, no Con.ID
 *       lookup and no SYSAP consent between the two. A message this node
 *       cannot route, cannot parse above the envelope, or has no listener
 *       for is still a message it RECEIVED, and the peer is told so. (That is
 *       the difference between a port and a SYSAP; conflating them is what
 *       froze the strawman.)
 *   (b) AN UNACKNOWLEDGED CIRCUIT DIES INSTEAD OF STALLING. If the PEER's
 *       recv_ack stops advancing while this node still has unacked messages,
 *       the retransmit ladder runs and then TIMVCFAIL breaks the circuit and
 *       re-forms it. A silent forever-stall is not an outcome this FSM has.
 *
 * The R1 test file asserts both directly, including the case where NO upper
 * layer is bound at all.
 *
 * ---------------------------------------------------------------------------
 * CREDIT -- one message, one credit, no invention
 *
 * p. 2-43/2-44: SCS flow control is a debit/credit account. The peer grants
 * this node its Send Credit in the START body (abs 95, byte-exact to SYSGEN
 * CLUSTER_CREDITS, spec §4(g)); each sequenced message this node sends debits
 * one, and each 0x48 credit-return the peer sends restores one -- "strict
 * 1-for-1: every sequenced message is answered by exactly one 0x48 returning
 * exactly one message's worth of credit" (spec §4(h)(3), GROUNDED).
 *
 * This layer therefore returns EXACTLY ONE credit per received sequenced
 * message, in the 0x48 that carries the ack, and NEVER also piggybacks one:
 * the abs-62 piggyback field belongs to the SCS connection layer (FC-P2.2's
 * per-CDT Pending Receive Credit), and returning a credit twice for one
 * message would inflate the peer's send window past what this node can take.
 * Credit conservation is an identity here, not an estimate.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS LAYER DOES **NOT** DO
 *
 * It does not build a sequenced message. It has no idea what a Con.ID, a
 * SYSAP or a lock is: the layer that owns the message builds the frame, and
 * the port stamps the circuit's sequence position into it on the way out and
 * again on every retransmission (vms_scs_seq_stamp). That is the VMS split --
 * p. 2-55/2-56 gives the port driver "VC dialogue, routing, credits, buffer
 * allocation, send/map routines" and leaves CDT/RDT/RSPID to SYS$SCS.
 * ========================================================================== */

/*
 * The largest frame the unacked ring stores. 204 = the SCA content class 190
 * plus the 14-byte Ethernet header -- the largest SCS MESSAGE class the wire
 * spec grounds (§4(d): the 190-content class is 17557/17557 of the Con.ID
 * layout; §4(d)'s own admitted classes are 58/62/66/86/94/110/190). The block
 * transfer classes are deliberately NOT covered: block transfer is FC-P6.1's
 * named-buffer service and does not ride this ring.
 */
#define PE_VC_FRAME_MAX 204u

/*
 * How many messages may be outstanding on one circuit. The peer's grant is
 * the real bound (CLUSTER_CREDITS, 10 in the lab); this is the ring that
 * holds them, sized with headroom so the CREDIT is what limits sending and
 * the ring never silently does. A send that would exceed either is REFUSED
 * and reported, never dropped.
 */
#define PE_VC_UNACKED_MAX 16u

/*
 * TIMVCFAIL: "the time required for an SCS virtual circuit failure to be
 * detected". A SYSGEN parameter; the glue converts it out of its SYSGEN unit
 * and puts milliseconds in pe_identity, so this FSM never does unit
 * arithmetic. The default below is OVMX's own choice for a port whose
 * SYSGEN value has not been loaded -- it is NOT a published VMS constant --
 * and it is the lab's TIMVCFAIL 1600 read as centiseconds.
 */
#define PE_TIMVCFAIL_DEFAULT_MS 16000u

/*
 * The retransmit cadence. No published document and no capture grounds a
 * sequenced-message retransmit interval (§4(k)'s measured 6.010 s is the
 * padded-HELLO SIZE probe, a different timer on a different frame class), so
 * this is OVMX's choice and is LABELLED as one: an eighth of TIMVCFAIL, i.e.
 * about eight attempts before the circuit is declared failed. Derived from
 * the one parameter the operator actually configures rather than adding a
 * new invented constant.
 */
#define PE_VC_RETRANSMIT_DIVISOR 8u
#define PE_VC_RETRANSMIT_MIN_MS  200u

/* One unacknowledged message: the frame as it went out, and its position. */
struct pe_vc_unacked {
	uint8_t  in_use;
	uint8_t  retransmits;     /* how many times this SAME seq went out */
	uint16_t seq;             /* the sequence it consumed, ONCE        */
	uint32_t len;
	uint32_t due_ms;          /* injected-clock deadline of the next try */
	uint8_t  frame[PE_VC_FRAME_MAX];
};

/* What a received sequence number IS, relative to this circuit's recv_seq.
 * Named so the table's handler reads as the rule it implements. */
enum pe_vc_seq_kind {
	PE_VC_SEQ_ANCHOR = 0,  /* the first on this circuit (§4(h)(4a))     */
	PE_VC_SEQ_NEXT,        /* recv_seq + 1: the normal case             */
	PE_VC_SEQ_DUP,         /* at or behind recv_seq: a peer retransmit  */
	PE_VC_SEQ_GAP          /* ahead by more than 1: p. 2-31, VC breaks  */
};

/* Why a circuit went down -- passed to the upper layer's vc_down and printed
 * on the console, so a stall is never silent. */
enum pe_vc_down_reason {
	PE_VC_DOWN_SEQ_GAP = 1,   /* p. 2-31 sequentiality guarantee lost   */
	PE_VC_DOWN_TIMVCFAIL,     /* no ack progress within TIMVCFAIL       */
	PE_VC_DOWN_CHANNEL,       /* the channel it rides stopped being ok  */
	PE_VC_DOWN_PEER_RESTART,  /* the peer sent a START on an open VC    */
	PE_VC_DOWN_PEER_GONE,     /* §4(O.30) last gasp                     */
	PE_VC_DOWN_SHUTDOWN       /* CLUSTER_STOP                           */
};

/*
 * One virtual circuit. Every field is either read off a real received frame,
 * counted from a real dispatch, or taken from this node's own loaded SYSGEN
 * identity. Nothing here has a default that stands in for a value the
 * executive has not got.
 */
struct pe_vc {
	uint8_t  in_use;
	uint8_t  state;            /* enum vms_pe_vc_state                  */
	uint8_t  channel;          /* the pe_channel index it rides         */
	uint8_t  config_round;     /* the round OUR last 0x41 carried       */

	/* ---- the peer's identity, ALL learned from its START/STACK body -- */
	vms_scs_sysid_t peer_sysid;
	uint8_t  peer_sysid_valid;
	uint8_t  peer_name[VMS_SCS_START_NODENAME_LEN];
	uint8_t  peer_name_valid;
	uint8_t  peer_swver[VMS_SCS_START_SWVER_LEN];
	uint8_t  peer_hwtype[VMS_SCS_START_HWTYPE_LEN];
	uint8_t  peer_ident_valid; /* a 106-byte START/STACK really arrived  */
	uint64_t peer_incarnation_time;   /* its boot time (spec §4(g) a80)  */

	/*
	 * The incarnation the PEER advertised for US, copied from the channel
	 * at formation time (spec §4(i).B). `_valid` clear means the channel
	 * never carried one -- and then no START is ever built.
	 */
	uint16_t echo_incarnation;
	uint8_t  echo_valid;
	uint8_t  pad0;

	/* ---- sequencing (spec §4(h)(4)) ---- */
	uint16_t send_seq;         /* OUR NEXT number, not the last one sent */
	uint16_t recv_seq;         /* highest peer send_seq taken, in order  */
	uint16_t peer_recv_ack;    /* the cumulative ack the PEER last sent  */
	uint8_t  recv_anchored;    /* a first sequenced message has arrived  */
	uint8_t  pad1;

	/* ---- flow control (p. 2-43/2-44) ---- */
	uint8_t  send_credit;      /* messages we may still send             */
	uint8_t  send_credit_max;  /* the peer's grant, from its START body  */
	uint8_t  recv_credit;      /* what we have granted and not returned  */
	uint8_t  recv_credit_max;  /* our own CLUSTER_CREDITS                */

	/* ---- the unacked ring, keyed by seq ---- */
	struct pe_vc_unacked ring[PE_VC_UNACKED_MAX];
	uint8_t  unacked;          /* entries in use                         */
	uint8_t  form_tries;       /* START/STACK attempts at this round     */
	uint8_t  pad2[2];

	/* ---- deadlines, injected clock, wrap-safe ---- */
	uint32_t form_due_ms;      /* next formation retry                   */
	uint32_t vcfail_due_ms;    /* TIMVCFAIL: no ACK PROGRESS by here     */
	uint8_t  vcfail_armed;
	uint8_t  pad3[3];

	/* ---- counters, every one from a real dispatch ---- */
	uint32_t starts_tx, starts_rx;
	uint32_t stacks_tx, stacks_rx;
	uint32_t acks_tx,   acks_rx;      /* the 0x41 round-2 ACK            */
	uint32_t msgs_tx,   msgs_rx;      /* sequenced messages              */
	uint32_t credit_tx, credit_rx;    /* the 0x48 short (ack + credit)   */
	uint32_t retransmits;             /* same seq re-sent                */
	uint32_t rx_dups;                 /* peer retransmits we absorbed    */
	uint32_t rx_gaps;                 /* p. 2-31 breaks                  */
	uint32_t implied_acks;            /* p. 2-16 opens                   */
	uint32_t opens;                   /* times this VC reached OPEN      */
	uint32_t downs;                   /* times it was torn down          */
	uint32_t send_refused_credit;     /* refused: no send credit         */
	uint32_t send_refused_ring;       /* refused: ring full              */
	uint8_t  last_down_reason;        /* enum pe_vc_down_reason, 0 = none */
	uint8_t  pad4[3];
};

/* ==========================================================================
 * 4. This node's honest identity on the wire
 *
 * Every field is filled by vms_pe.c (FC-P0.9) from REAL executive state -- the
 * interface's hardware address read back through the seam, the SYSGEN
 * parameters, the CLUSTER_AUTHORIZE record. Nothing in this struct has a
 * default that stands in for a value the executive has not got: an absent value
 * carries a `_valid` companion, and what goes on the wire in its place is a
 * documented zero that is COUNTED, never a byte copied out of somebody else's
 * capture.
 * ========================================================================== */
struct pe_identity {
	/* The interface's REAL hardware address (spec SS4(b) abs 120, and the
	 * Ethernet source of every frame). exec_lan_hwaddr, through the glue. */
	uint8_t  hw_mac[VMS_ETH_ADDR_LEN];
	uint8_t  hw_mac_valid;

	/* SCSNODE, ASCII space-padded, as SYSGEN loaded it (spec SS4(a) abs 40). */
	uint8_t  scsnode_len;
	uint8_t  scsnode[VMS_HELLO_NODENAME_MAX];

	/* This node's cluster-LOGICAL address aa:00:04:00:<LE16(SCSSYSTEMID)>
	 * (spec SS4(a) abs 24). Built by pe_fsm_init from SCSSYSTEMID; invalid --
	 * and then NO frame is emitted at all -- if SCSSYSTEMID is absent or does
	 * not fit the two bytes SS4(a) grounds. */
	uint8_t  lavc[VMS_ETH_ADDR_LEN];
	uint8_t  lavc_valid;

	/* The cluster HELLO multicast group AB-00-04-01-<group>, assembled by the
	 * glue from the CLUSTER_AUTHORIZE group number (spec SS3). */
	uint8_t  mcast[VMS_ETH_ADDR_LEN];
	uint8_t  mcast_valid;

	/*
	 * The connect/join nonce (spec SS4(a) abs 68). ZERO on a multicast HELLO
	 * is GROUNDED; on a directed HELLO the real cluster carries a shared
	 * token whose provenance is design SS5.3's OPEN QUESTION, measured by
	 * FC-P0.13. Until that lands the executive has no such token, so
	 * `join_nonce_valid` is 0, a zero goes out, and pe_fsm.nonce_absent
	 * counts every frame that went out without one. The strawman daemon baked
	 * a captured VAX's token in as a constant; this file will not.
	 */
	uint8_t  join_nonce[VMS_DISC_NONCE_LEN];
	uint8_t  join_nonce_valid;

	/*
	 * The two spans spec SS4(a) records as PRESENT but does not publish the
	 * meaning of (abs 47-63 "capability/version-ish", abs 64-67 "unknown").
	 * The codec deliberately refuses to supply a default for either, so they
	 * come from here, and today the executive's honest value for both is
	 * zero: OVMX does not know what it would be asserting.
	 */
	uint8_t  cap_span[VMS_DISC_CAPSPAN_LEN];
	uint8_t  reserved_64[VMS_DISC_RESERVED64_LEN];

	/*
	 * The largest SCA content this port may put on the wire: NISCS_MAX_PKTSZ
	 * + 2, already clamped to the interface MTU by the glue (the FSM does no
	 * MTU arithmetic -- it has no interface). 0 means "size verification is
	 * not attempted", which is honest on a port whose MTU is unknown.
	 */
	uint16_t max_sca_len;

	/* 0 selects the documented default above. A SYSGEN value always wins. */
	uint32_t hello_interval_ms;
	uint32_t listen_timeout_ms;

	/* ------------------------------------------------------------------
	 * Added by FC-P1.2: what the VIRTUAL CIRCUIT's formation body asserts
	 * about this system (spec §4(g) phase 2). Every one of these is a
	 * per-node or per-boot value the glue reads out of real executive
	 * state; NONE of them has a fallback here, because every one of them
	 * is a claim about who this node is.
	 * ------------------------------------------------------------------ */

	/*
	 * abs 72, 8 ASCII: the software version this node BROADCASTS. OVMX
	 * broadcasts its own honest "VMX Vx.y" (the honest-OS-identity
	 * ruling); it is a field, not a constant, precisely so no capture's
	 * "VMS V7.3" can be baked in anywhere in this stack.
	 */
	uint8_t  sw_version[VMS_SCS_START_SWVER_LEN];
	uint8_t  sw_version_valid;

	/* abs 88, 4 ASCII: this node's hardware class ("VAX ", "AXP ", ...),
	 * which differs per substrate and is therefore never a constant. */
	uint8_t  hw_type[VMS_SCS_START_HWTYPE_LEN];
	uint8_t  hw_type_valid;

	/* abs 95: SYSGEN CLUSTER_CREDITS -- the Send Credit this node GRANTS
	 * the peer (p. 2-43). 0 is a legitimate configured value and is
	 * honoured; the flag distinguishes it from "not loaded". */
	uint8_t  cluster_credits;
	uint8_t  cluster_credits_valid;

	/*
	 * abs 80: THIS SYSTEM'S INCARNATION -- a VMS absolute-time quadword,
	 * the time this system was BOOTED (spec §4(g), GROUNDED four ways by
	 * vms-2f3). It arrives from the executive; it is NEVER sampled here
	 * and NEVER defaulted. A node that advertises an incarnation it did
	 * not boot with earns a CLUEXIT bugcheck on the surviving side after a
	 * reconnect (VSI OpenVMS Cluster Systems App. C.7.1) -- which is
	 * exactly what OVMX did for six days by replaying a captured value.
	 * With the flag clear this FSM forms no circuit and says so.
	 */
	uint64_t incarnation_time;
	uint8_t  incarnation_time_valid;
	uint8_t  pad_vc[3];

	/* 0 selects the documented defaults. A SYSGEN value always wins; the
	 * glue converts TIMVCFAIL out of its SYSGEN unit, so no unit
	 * arithmetic happens inside the FSM. */
	uint32_t timvcfail_ms;
	uint32_t vc_retransmit_ms;
};

/* ==========================================================================
 * 5. One channel
 *
 * Everything here is either read off a real received frame or counted from a
 * real dispatch. No field is a placeholder, and every learned identity carries
 * its validity flag (INV-6).
 * ========================================================================== */
struct pe_channel {
	uint8_t  in_use;        /* 0 = a free slot, not "a channel to 00:00:.." */
	uint8_t  state;         /* enum vms_pe_channel_state */
	uint8_t  pad0[2];

	/* ---- identity, all learned from received frames ---- */
	uint8_t  remote_mac[VMS_ETH_ADDR_LEN];   /* the frame's Ethernet source */
	uint8_t  remote_lavc[VMS_ETH_ADDR_LEN];  /* abs 24 of ITS HELLO (SS4(a).0) */
	uint8_t  remote_lavc_valid;
	uint8_t  remote_name_len;
	uint8_t  remote_name[VMS_HELLO_NODENAME_MAX];
	uint8_t  remote_sysid_valid;
	uint8_t  pad1;
	vms_scs_sysid_t remote_sysid;            /* from the LOGICAL address     */

	/*
	 * The incarnation THIS PEER advertises FOR US (spec SS4(i).B). Real wire
	 * data; FC-P1.2's START must echo exactly this. `_valid` is 0 until a
	 * directed HELLO actually carried one -- a zero here is "never
	 * advertised", never "incarnation zero".
	 */
	uint16_t peer_incarnation;
	uint8_t  peer_incarnation_valid;
	uint8_t  pad2;

	/* ---- the size verification (SS4(k)) ---- */
	uint16_t verified_pktsz;  /* SCA content a b4 CONFIRMED. 0 = not proven */
	uint16_t probe_sca_len;   /* the probe in flight. 0 = none outstanding  */
	uint8_t  probe_rung;      /* index into pe_probe_ladder                 */
	uint8_t  probe_tries;     /* attempts made at this rung                 */
	uint8_t  probe_exhausted; /* ladder walked out; counted, never invented */
	uint8_t  pad3;
	uint32_t probe_due_ms;    /* injected-clock deadline of the retransmit  */

	/* ---- liveness, on the injected clock, compared wrap-safely ---- */
	uint32_t last_rx_ms;
	uint32_t deadline_ms;     /* last_rx_ms + the listen timeout            */

	/* ---- counters, every one from a real dispatch ---- */
	uint32_t hello_rx;
	uint32_t hello_tx;
	uint32_t b2_rx;
	uint32_t b3_rx;
	uint32_t b3_tx;
	uint32_t b4_rx;
	uint32_t b4_tx;
	uint32_t padded_rx;
	uint32_t padded_tx;
	uint32_t resets;          /* peer re-formed this channel (SS4(i).B)     */
};

/* ==========================================================================
 * 6. The FSM context
 *
 * No globals (design SS3.9 rule 3). The scratch frame buffer lives here so the
 * FSM has no allocator at all: a host test binds ops->alloc and ops->free to
 * NULL and the tests crash rather than quietly pass if this file ever reaches
 * for one.
 * ========================================================================== */
struct pe_fsm {
	struct pe_identity     id;
	const struct pe_ops   *ops;

	/* SCSSYSTEMID as pe_fsm_init was given it. The channel layer needs
	 * only the cluster-LOGICAL address built from it; the VC's START body
	 * carries the number itself at abs 60 (spec §4(g) phase 2), so it is
	 * kept rather than re-derived from the address. */
	vms_scs_sysid_t        sysid;

	uint8_t  running;         /* the cadence beat is armed */
	uint8_t  link_up;
	uint8_t  pad0[2];

	uint32_t n_channels;      /* high-water: slots 0..n_channels-1 may be used */
	struct pe_channel ch[PE_MAX_CHANNELS];

	/* ---- port-wide counters ---- */
	uint32_t mcast_hello_tx;
	uint32_t rx_frames;         /* frames handed to pe_fsm_rx                */
	uint32_t rx_not_sca;        /* not ethertype 0x6007                      */
	uint32_t rx_unclassified;   /* the codec could not name the class        */
	uint32_t rx_not_for_us;     /* addressed to neither us nor the group     */
	uint32_t rx_parse_failed;   /* classified, then failed to decode         */
	uint32_t rx_solicit;        /* SS4(c): counted, NEVER answered (P6/P7)   */
	uint32_t rx_no_slot;        /* channel table full: refused, not recycled */
	uint32_t ignored_events;    /* no grounded edge for [state][event]       */
	uint32_t nonce_absent;      /* directed frames sent with no credential   */
	uint32_t tx_errors;         /* ops->send returned non-zero               */
	uint32_t last_gasps_built;

	/* ---- FC-P1.2: the virtual-circuit half ----
	 *
	 * The VC table is BOUND, not embedded: one circuit carries a ring of
	 * whole frames, so the table's size is a deployment decision (a
	 * two-node harness wants two, a 96-system cluster wants 96) and it
	 * belongs to the glue that knows how much memory it may take. The FSM
	 * itself still allocates NOTHING -- ops->alloc stays unused and NULL
	 * in every test, which is a harder guarantee than a counter.
	 */
	struct pe_vc         *vc;        /* bound table, or NULL              */
	uint32_t              n_vc_slots;/* its capacity                      */
	uint32_t              n_vcs;     /* high-water of used slots          */
	const struct pe_upper_ops *upper;/* SCS, or NULL (see the ack rule)   */

	uint32_t vc_ignored_events; /* [vc state][event] cell with no edge    */
	uint32_t vc_no_slot;        /* the VC table was full: refused         */
	uint32_t vc_no_incarnation; /* §4(i).B echo absent: NO START built    */
	uint32_t vc_no_identity;    /* incarnation time / clock absent        */
	uint32_t vc_rx_no_circuit;  /* SCS frame with no circuit to take it   */
	uint32_t vc_rx_no_channel;  /* SCS frame from an unknown station      */
	uint32_t vc_rx_parse_failed;/* classified SCS, then failed to decode  */
	uint32_t vc_reformations;   /* circuits re-formed after a failure     */
	/*
	 * Received, ACKNOWLEDGED, and not handed upward: no upper layer bound,
	 * or a class whose Con.ID location the codec does not ground (§4(d)
	 * leaves every length class but 190/110/66/62 undecoded). The ack went
	 * out either way -- that is §3b(a) -- so this counter measures what
	 * this node cannot yet ROUTE, never a circuit that stalled. FC-P2.1
	 * grounds more classes and drives it down.
	 */
	uint32_t vc_rx_undelivered;

	/* The one frame buffer. Sized for the largest frame SS4(k) grounds. */
	uint8_t  scratch[VMS_HELLO_PADDED_MAX_FRAME];
};

/* ==========================================================================
 * 7. Lifecycle
 * ========================================================================== */

/*
 * Bind the FSM to its identity and its ops. `sysid` is SCSSYSTEMID as SYSGEN
 * loaded it; the cluster-LOGICAL address is built from it here through the
 * codec's own helper (spec SS4(a)), and if it does not fit the two bytes the
 * wire grounds, `id.lavc_valid` stays 0 -- the port then emits NOTHING rather
 * than a truncated address, which is the honest end of the road.
 *
 * Arms no timer. Returns 0, or -1 if `f` or `ops` is NULL.
 */
int pe_fsm_init(struct pe_fsm *f, const struct pe_identity *id,
		vms_scs_sysid_t sysid, const struct pe_ops *ops);

/* Arm / cancel the port-wide HELLO cadence beat. Idempotent. */
void pe_fsm_start(struct pe_fsm *f);
void pe_fsm_stop(struct pe_fsm *f);

/* ==========================================================================
 * 8. Events
 * ========================================================================== */

/*
 * A received frame. The CODEC classifies it and decides which pe_event it is
 * (design: "the codec -- never the FSM -- decides which event a received frame
 * is"); this function then finds or creates the channel for the sending
 * station, learns what the frame honestly carries, and dispatches through the
 * table. A frame that is not ours, not SCA, or not classifiable is COUNTED and
 * dropped -- never guessed at.
 *
 * Returns the action the caller owes the layers above.
 */
enum pe_channel_action pe_fsm_rx(struct pe_fsm *f, const uint8_t *frame,
				 uint32_t len);

/*
 * The port-wide cadence beat (PE_TIMER_HELLO). Emits the multicast HELLO, then
 * gives every live channel its own tick -- the directed keepalive b3, the
 * SS4(k) probe retransmit/down-step, and the SS4(M) listen timeout -- and
 * re-arms itself. Fills up to `max` per-channel actions and returns how many.
 *
 * ONE beat drives all of it, and every deadline is a wrap-safe comparison
 * against ops->now_ms, exactly as FC-P3.6's once-a-second reconnect beat does.
 * A 49.7-day uptime rollover therefore cannot make a deadline unreachable.
 */
struct pe_channel_rec {
	uint32_t channel_index;
	uint8_t  action;      /* enum pe_channel_action */
	uint8_t  pad[3];
};

uint32_t pe_fsm_tick(struct pe_fsm *f, struct pe_channel_rec *out, uint32_t max);

/* One channel's own timer (PE_TIMER_CHANNEL, key = the channel index). Same
 * per-channel work the beat does, for a glue that prefers a timer per channel. */
enum pe_channel_action pe_fsm_channel_timer(struct pe_fsm *f, uint32_t index);

/*
 * Post a NON-FRAME event to one channel -- a timer that fired, a link fact, a
 * shutdown. This is the glue's way in (the fork thread drains a work queue of
 * exactly these) and it is also how the host tests reach every cell of the
 * table that no received frame can produce, which is what makes "every
 * transition is a test" mean the whole table and not just the reachable half.
 *
 * The DISCOVERY-family RX_* events are deliberately NOT accepted here: one
 * without the frame that justified it is an event with no evidence behind it,
 * and this function returns PE_CH_ACT_NONE for it. Received frames go through
 * pe_fsm_rx(), where the codec decides what they are. A VC-layer event (FC-P1.2's
 * START/STACK/ACK/SEQMSG/DATAGRAM/CREDIT) IS accepted and lands on an empty cell,
 * so f->ignored_events records that the channel layer answers nothing above the
 * circuit rather than silently swallowing it.
 */
enum pe_channel_action pe_fsm_event(struct pe_fsm *f, uint32_t index,
				    enum pe_event ev);

/* The interface came up / went down. LINK_DOWN closes every channel: with no
 * link there is no connectivity to any station, and holding a "verified"
 * channel across it would be a claim the executive cannot support. */
void pe_fsm_link_up(struct pe_fsm *f);
uint32_t pe_fsm_link_down(struct pe_fsm *f, struct pe_channel_rec *out,
			  uint32_t max);

/* CLUSTER_STOP: cancel the beat and close every channel. Emits nothing -- the
 * last gasp is a separate, deliberate act (below). */
void pe_fsm_shutdown(struct pe_fsm *f);

/*
 * The SS4(O.30) clean-leave last gasp: a MULTICAST HELLO differing from the
 * periodic one at exactly two semantic fields -- abs 30 a0 -> b1, and the
 * cluster nonce at abs 68 (GROUNDED byte-exact on two real-VAX clean leaves;
 * within each capture the combination appears exactly ONCE).
 *
 * It lives here because the port owns the HELLO frame class, and FC-P3.6's
 * reconnect FSM already hands its caller a CNXMAN_CSB_ACT_LAST_GASP record with
 * nothing to build the frame -- this closes that. It is a deliberate call, not
 * a table edge: p. 7-29 emits it when the SYSTEM leaves, and only then.
 *
 * HONESTY: SS4(O.30) says the departing node authenticates the datagram with
 * the cluster token. If the executive has no token (design SS5.3 is open), the
 * frame still carries the b1 marker -- which is the departure semantics -- with
 * a zero nonce, and pe_fsm.nonce_absent counts it. OVMX announces its own
 * departure honestly rather than replaying somebody else's credential.
 *
 * Returns 0 on success, or -1 when there is no identity to send from.
 */
int pe_fsm_send_last_gasp(struct pe_fsm *f);

/* ==========================================================================
 * 8b. The VIRTUAL CIRCUIT surface (FC-P1.2)
 * ========================================================================== */

/*
 * Bind the circuit table and the layer above, AFTER pe_fsm_init (which zeroes
 * the whole context, bindings included). Both are the glue's objects:
 * `vcs` is `n` zeroed pe_vc slots (the port's PB set) and `upper` is SCS.
 *
 * NEITHER is required. With no table bound the port runs as FC-P0.8 shipped
 * it -- channels only, no circuit ever formed, every SCS frame counted in
 * vc_rx_no_circuit. With a table but no upper layer bound the circuits form,
 * sequence, acknowledge and retransmit exactly as they otherwise would, and
 * received messages are counted instead of delivered: acknowledgement is a
 * TRANSPORT fact and does not depend on anyone being home (see §3b(a)).
 */
void pe_fsm_bind_vcs(struct pe_fsm *f, struct pe_vc *vcs, uint32_t n);
void pe_fsm_set_upper(struct pe_fsm *f, const struct pe_upper_ops *upper);

/* Why a send was refused. Negative so `if (rc)` reads as failure; the glue
 * maps each to the SS$_ status vms_pe.h's pe_send_msg promises (kernel-core
 * cluster headers stay free of SS$_ definitions -- design §3.2.2). */
enum pe_vc_send_status {
	PE_VC_SEND_OK        =  0,
	PE_VC_SEND_NOCIRCUIT = -1,  /* no OPEN circuit to that system        */
	PE_VC_SEND_NOCREDIT  = -2,  /* the peer's Send Credit is exhausted   */
	PE_VC_SEND_RINGFULL  = -3,  /* unacked ring full (credit says other) */
	PE_VC_SEND_BADFRAME  = -4,  /* not a stampable sequenced SCS frame   */
	PE_VC_SEND_TOOBIG    = -5,  /* larger than PE_VC_FRAME_MAX           */
	PE_VC_SEND_TXFAIL    = -6   /* ops->send failed; the seq is HELD     */
};

/*
 * Send one sequenced message on the circuit to `dst`.
 *
 * `frame` is a COMPLETE SCS frame the calling layer built through its own
 * codec entry -- addressing, Con.ID pair, SYSAP body and all. This function
 * assigns it the circuit's next sequence, stamps recv_ack/send_seq/mirror
 * into it (vms_scs_seq_stamp), transmits it and keeps a copy for
 * retransmission. See §3b "WHAT THIS LAYER DOES NOT DO": the port owns the
 * sequence, the caller owns the message.
 *
 * THE SEQUENCE IS CONSUMED ONLY BY A FRAME THAT WENT OUT. A frame that fails
 * the class gate consumes nothing; a frame ops->send rejects keeps its
 * sequence AND its ring entry, so the retransmit ladder fills the position
 * rather than leaving the hole that would break the peer's circuit (§3b(1)).
 *
 * `pe_vc_addr` hands back the four addresses that frame must carry, read from
 * the circuit's OWN channel -- the peer's hardware MAC and cluster-LOGICAL
 * address as they were learned from its frames, and this node's own two. It
 * exists so the layer above builds with REAL addressing instead of inventing
 * any part of it. 0 on success, -1 if there is no such circuit.
 */
int pe_vc_send_frame(struct pe_fsm *f, vms_scs_sysid_t dst,
		     const uint8_t *frame, uint32_t len);
int pe_vc_addr(struct pe_fsm *f, vms_scs_sysid_t dst, struct vms_scs_addr *out);

/*
 * One circuit's timer beat (PE_TIMER_RETRANSMIT, key = the circuit index).
 * Every deadline inside is a wrap-safe comparison against ops->now_ms, so
 * calling it early, late or twice is harmless -- which is what lets
 * pe_fsm_tick drive them all from the one port beat as well, and why this
 * handler also tests the TIMVCFAIL deadline rather than needing its timer to
 * have fired. A glue that arms PE_TIMER_VCFAIL separately posts
 * PE_EV_TIMER_VCFAIL through pe_fsm_vc_event; both reach the same test.
 */
void pe_fsm_vc_timer(struct pe_fsm *f, uint32_t index);

/* Post a non-frame event to one circuit -- the glue's way in, and the tests'
 * way to reach the cells no received frame can produce. */
void pe_fsm_vc_event(struct pe_fsm *f, uint32_t index, enum pe_event ev);

/* The circuit at `index`, or NULL past the end / for a free slot; and the
 * circuit to a system, by the SCSSYSTEMID its own frames carried. */
struct pe_vc *pe_fsm_vc_at(struct pe_fsm *f, uint32_t index);
struct pe_vc *pe_fsm_vc_by_sysid(struct pe_fsm *f, vms_scs_sysid_t sysid);

/* Project one circuit into the frozen cross-substrate view (INV-6: what was
 * never learned stays zero with its flag clear). */
void pe_fsm_vc_project(const struct pe_fsm *f, const struct pe_vc *vc,
		       struct vms_pe_vc_view *out);

const char *pe_vc_state_name(enum vms_pe_vc_state s);
const char *pe_vc_down_reason_name(enum pe_vc_down_reason r);

/* ==========================================================================
 * 9. Readback -- the same struct the diagnostics ioctl projects (INV-6)
 * ========================================================================== */

/* The channel at `index`, or NULL past the end / for a free slot. */
struct pe_channel *pe_fsm_channel_at(struct pe_fsm *f, uint32_t index);

/* The channel for a station, by its hardware address. NULL if unknown. */
struct pe_channel *pe_fsm_channel_by_mac(struct pe_fsm *f,
					 const uint8_t mac[VMS_ETH_ADDR_LEN]);

/* Project one channel into the frozen cross-substrate view. A value the
 * executive has not learned stays zero AND its flag stays clear, so a reader
 * blanks the column instead of printing a number nobody claimed. */
void pe_fsm_channel_project(const struct pe_channel *ch,
			    struct vms_pe_channel_view *out);

/* Names, for the console and for a test's failure message. */
const char *pe_channel_state_name(enum vms_pe_channel_state s);
const char *pe_channel_action_name(enum pe_channel_action a);

#endif /* OVMX_VMS_PE_FSM_H */
