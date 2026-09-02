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
