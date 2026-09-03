/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_pe.h - the LAN port layer (PEDRIVER role, PEA0:): its injected ops, its
 * event vocabulary, and the port services SCS calls (FC-P0.1).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.1/SS3.2 (the port owns
 * HELLO/SOLICIT discovery, channels, virtual circuits with sequencing and
 * retransmit, and the three port services), SS3.9 (pure FSM + injected ops +
 * glue + snapshot). Wire spec: docs/cluster-protocol-spec.md SS4(a)-(c), SS4(g),
 * SS4(i), SS4(k).
 *
 * THE LAYER IN ONE PARAGRAPH. The port owns the LAN device for ethertype 0x6007.
 * It sends a HELLO on a cadence, answers a SOLICIT, verifies a CHANNEL to each
 * remote station through the b2/b3/b4 handshake, and over a verified channel
 * forms a VIRTUAL CIRCUIT with the remote SYSTEM (START/STACK/ACK with the
 * incarnation echo). Above the VC it offers three services -- datagram,
 * sequenced message, block transfer -- and delivers what arrives UPWARD by
 * (remote system, destination Con.ID). It knows nothing about SYSAPs,
 * membership, quorum or locks.
 *
 * THE THREE FILES (design SS3.9), of which this header is the interface:
 *   vms_pe_fsm.c   pure: the channel and VC tables, one small handler per
 *                  transition, every action emitted through `struct pe_ops`.
 *                  Host-unit-testable and simulator-runnable with NO kernel.
 *   vms_pe.c       glue: owns the objects, binds pe_ops to the real seam
 *                  (exec_lan_xmit, the fork module's cf_timer_*), registers
 *                  PEA0: in vms_devtab, fills the snapshot.
 *   vms_cluster_snapshot.h   the read-only view both of them project into.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_PE_H
#define OVMX_VMS_PE_H

#include "vms_cluster.h"
#include "vms_cluster_snapshot.h"

struct vms_pe;      /* opaque: the port's objects are private to vms_pe.c */
struct vms_pe_fsm;  /* opaque: the pure state machine, private to vms_pe_fsm.c */
/* FC-P6.1: one block transfer as the SYSAP describes it. DEFINED in
 * vms_pe_fsm.h SS3c (which includes this header, not the other way round), so
 * this header stays includable alone and names it only through a pointer. */
struct pe_blk_xfer;

/* ==========================================================================
 * 1. Timers the port arms
 *
 * Named, not numbered-at-the-call-site: a table-driven FSM arms a timer by
 * IDENTITY, and the fork module's cf_timer_* wrappers map the identity to one
 * exec_timer_t. Re-arming an armed timer moves it; it never stacks.
 * ========================================================================== */
enum pe_timer {
	PE_TIMER_HELLO      = 0,  /* the ~2 s HELLO cadence */
	PE_TIMER_CHANNEL    = 1,  /* channel listen/verify timeout, per channel */
	PE_TIMER_RETRANSMIT = 2,  /* unacked sequenced message, per VC */
	PE_TIMER_VCFAIL     = 3,  /* TIMVCFAIL, per VC */
	PE_TIMER__COUNT
};

/* ==========================================================================
 * 2. The injected ops -- the ONLY way the pure FSM reaches the world
 *
 * Design SS3.9: "the actions it emits through an injected struct <layer>_ops
 * { send, arm_timer, cancel_timer, now, log, alloc, free }". Production binds
 * these to the seam; a host test binds them to recorders; the rung-2 simulator
 * binds them to a virtual LAN and a virtual clock. `now_ms` is here for rule 6
 * -- an FSM NEVER reads the clock itself, so a test drives time.
 * ========================================================================== */
struct pe_ops {
	/* Transmit ONE complete Ethernet frame (byte 0 = destination MAC).
	 * Returns 0 or an SS$_ status. Production: exec_lan_xmit. */
	int  (*send)(void *ctx, const uint8_t *frame, uint32_t len);

	/* Arm / cancel a named timer. `key` distinguishes per-object timers
	 * (which channel, which VC); it is the object's index, never a pointer,
	 * so a recorded test transcript is comparable. */
	void (*arm_timer)(void *ctx, enum pe_timer which, uint32_t key, uint32_t ms);
	void (*cancel_timer)(void *ctx, enum pe_timer which, uint32_t key);

	/* The monotonic millisecond clock (exec_ticks_ms in production, the
	 * virtual clock in the simulator). */
	uint32_t (*now_ms)(void *ctx);

	/*
	 * Added by FC-P1.2, additively and at the end of the struct.
	 *
	 * The VMS ABSOLUTE-TIME clock (exec_time_now_vms in production;
	 * design §17): 100 ns units since 17-NOV-1858. It is a SEPARATE
	 * primitive from now_ms because the wire needs both and they are not
	 * convertible: now_ms is a monotonic tick with an arbitrary origin,
	 * while the START/STACK body carries two REAL VMS quadwords -- this
	 * system's incarnation (its boot time, abs 80) and the time the frame
	 * was composed (abs 112, spec §4(g) phase 2).
	 *
	 * The second one is why this is an OP and not a field: spec §4(g)'s
	 * own grounding is a NEGATIVE -- "no real node ever sends a stale
	 * one" -- so it must be sampled per frame. A NULL here means the
	 * executive has no absolute-time clock bound, and the VC then forms
	 * NO circuit at all rather than stamping a zero that decodes as
	 * 17-NOV-1858 (INV-6: honest absence, never a placeholder; the
	 * campaign's replayed 26-JUL-2026 incarnation is the bug this
	 * prevents, spec §4(g) CORRECTION vms-2f3).
	 */
	uint64_t (*now_vms)(void *ctx);

	/* One OPCOM-class line. Production: exec_console_printf. */
	void (*log)(void *ctx, const char *msg);

	/* Object allocation for channels/VCs/frame buffers. Production:
	 * exec_zalloc/exec_free; host tests use the C library. Never called
	 * from a receive or timer callback (CONTRACT RULES 1 and 2). */
	void *(*alloc)(void *ctx, uint32_t n);
	void  (*free)(void *ctx, void *p);

	void *ctx;
};

/* ==========================================================================
 * 3. The event vocabulary
 *
 * The FSM is a table `handlers[state][event]` (design SS3.9 rule 1: a table
 * entry, not a switch ladder). Every event below is a GROUNDED frame class or a
 * timer/link fact -- no event exists for a frame class OVMX has not observed.
 * FC-P0.8 (channels) and FC-P1.2 (virtual circuits) own the tables; adding an
 * event here is an additive change those items make, and the codec -- never the
 * FSM -- decides which event a received frame is.
 * ========================================================================== */
enum pe_event {
	/* Discovery and channel verification (spec SS4(a)-(c), SS4(k)). */
	PE_EV_RX_HELLO        = 0,   /* multicast or directed HELLO */
	PE_EV_RX_SOLICIT      = 1,
	PE_EV_RX_VERIFY_B2    = 2,   /* the b2/b3/b4 channel-verify ladder */
	PE_EV_RX_VERIFY_B3    = 3,
	PE_EV_RX_VERIFY_B4    = 4,

	/* Virtual-circuit formation (spec SS4(g)/(i)). */
	PE_EV_RX_START        = 5,
	PE_EV_RX_STACK        = 6,
	PE_EV_RX_ACK          = 7,

	/* Data on a formed circuit. */
	PE_EV_RX_SEQMSG       = 8,   /* sequenced message (carries a Con.ID pair) */
	PE_EV_RX_DATAGRAM     = 9,   /* datagram (no Con.ID pair) */
	PE_EV_RX_CREDIT       = 10,  /* port-level credit return */

	/* Timers (CONTRACT RULE 2: posted by the callback, run here). */
	PE_EV_TIMER_HELLO     = 11,
	PE_EV_TIMER_CHANNEL   = 12,
	PE_EV_TIMER_RETRANSMIT = 13,
	PE_EV_TIMER_VCFAIL    = 14,

	/* Local facts. */
	PE_EV_LINK_UP         = 15,
	PE_EV_LINK_DOWN       = 16,
	PE_EV_SEND_REQUEST    = 17,  /* an upper layer asked to send */
	PE_EV_SHUTDOWN        = 18,  /* CLUSTER_STOP / last gasp */

	/*
	 * Added by FC-P0.8, additively and at the end (the numbering above is
	 * unchanged), as the paragraph opening this section sanctions. Both are
	 * GROUNDED frame facts, not invented vocabulary:
	 *
	 *   NEW_INCARNATION  a directed HELLO from a peer carries an abs-92
	 *                    incarnation different from the one this channel has
	 *                    recorded. Spec SS4(i).B: that number is the sender's
	 *                    count of how many times the RECEIVER has re-formed
	 *                    this channel, so a change means the peer regards the
	 *                    previous generation as gone.
	 *   RX_LAST_GASP     a multicast HELLO carrying the SS4(O.30) departure
	 *                    marker at abs 30 (a0 -> b1). GROUNDED byte-exact on
	 *                    two real-VAX clean leaves; it is what lets CNXMAN
	 *                    take p. 7-29's "announced departure" path instead of
	 *                    waiting out the whole reconnect period.
	 */
	PE_EV_RX_NEW_INCARNATION = 19,
	PE_EV_RX_LAST_GASP       = 20,

	/*
	 * Added by FC-P1.2, additively and at the end (19/20 unchanged). Both
	 * are LOCAL facts -- the channel layer's own conclusions -- and they
	 * are events rather than a function call because the VIRTUAL CIRCUIT
	 * rides the channel: a circuit may be formed only over a channel the
	 * b2/b3/b4 ladder has VERIFIED, and a channel that stops being
	 * verified takes its circuit with it (design §3.4; the port "forms a
	 * VIRTUAL CIRCUIT with the remote SYSTEM" over a verified channel).
	 *
	 *   CHANNEL_UP    the channel reached b4: PE_CH_ACT_VERIFIED. This is
	 *                 what starts a VC formation, and the ONLY thing that
	 *                 does -- there is no timer that opens a circuit over
	 *                 an unverified channel.
	 *   CHANNEL_DOWN  PE_CH_ACT_RESET or PE_CH_ACT_LOST: the peer
	 *                 re-formed the channel (spec §4(i).B) or the listen
	 *                 timeout expired (spec §4(M)). Either way the circuit
	 *                 built on the old generation is stale and is torn
	 *                 down; it re-forms on the next CHANNEL_UP.
	 */
	PE_EV_CHANNEL_UP         = 21,
	PE_EV_CHANNEL_DOWN       = 22,

	PE_EV__COUNT
};

/* ==========================================================================
 * 4. Upward delivery -- the (SB, Con.ID) boundary
 *
 * The port delivers a received sequenced message to the upper layer by the
 * REMOTE SYSTEM it came from and the DESTINATION Con.ID the message names. That
 * Con.ID is read by a codec accessor out of the received envelope
 * (SCS$L_DST_CONID, a longword in the published $SCSDEF definitions) -- the port
 * never invents one, and a frame class that carries no Con.ID pair is delivered
 * through `datagram` instead, not through `message` with a zero. SCS owns the
 * demux from Con.ID to CDT; the port owns nothing above the circuit.
 * ========================================================================== */
struct pe_upper_ops {
	void (*message)(void *ctx, vms_scs_sysid_t from, vms_conid_t dst_conid,
			const uint8_t *body, uint32_t len);
	void (*datagram)(void *ctx, vms_scs_sysid_t from,
			 const uint8_t *body, uint32_t len);
	/*
	 * Circuit lifecycle: SCS must tear its CDTs down when a VC is lost, and
	 * CNXMAN must see it as a connectivity change.
	 *
	 * `vc_down` IS THE SEAM DESIGN SS3.2.5 NAMES (FC-P1.9 made it
	 * load-bearing): "on PE_VC_DOWN_* the port raises vc_down(sysid, reason)
	 * to SCS; SCS moves every CDT on that SB to CLOSED with reason
	 * path-lost, discards their credit ledgers, fails pending sends with
	 * SS$_PATHLOST, and calls each SYSAP's disconnected(local_conid,
	 * reason)". SCS never retries a message across a break and never
	 * re-opens a connection itself -- CNXMAN's recnx_fsm is the SYSAP that
	 * reconnects (spec SS4(aa)). Port-level retransmission is INVISIBLE to
	 * SCS: it sees an ordered, gap-free stream, and a sequenced message
	 * spends its credit exactly once at scs_send_msg however many times the
	 * port re-sent it.
	 *
	 * `reason` is an `enum pe_vc_down_reason` (vms_pe_fsm.h) -- what the
	 * pure FSM actually holds and can defend. The GLUE maps it to the SS$_
	 * status a user-mode reader sees; the FSM has no SS$_ definitions
	 * (design SS3.2.2 keeps kernel-core cluster headers free of them) and
	 * inventing one inside it would be a value nothing in the executive
	 * produced.
	 */
	void (*vc_up)(void *ctx, vms_scs_sysid_t peer);
	void (*vc_down)(void *ctx, vms_scs_sysid_t peer, uint32_t reason);
	void *ctx;

	/*
	 * Added by FC-P6.1, additively and at the end of the struct.
	 *
	 * BLOCK-TRANSFER COMPLETION -- reported AFTER the bytes are already in
	 * the named buffer, and describing only what actually landed. `name` is
	 * the SYSAP's own buffer name (the one it registered and told the peer),
	 * `offset`/`len` is the span this frame filled, and `bytes_remaining` is
	 * the transfer's own down-counter as the frame carried it -- so a SYSAP
	 * knows the transfer is complete when it reaches `len` and never has to
	 * guess. NULL is legitimate: the port still takes the bytes into the
	 * buffer and still counts the frame, because a delivery notification is
	 * an application fact and the transfer is a transport one (SS3b(a)).
	 */
	void (*block_data)(void *ctx, vms_scs_sysid_t from, uint32_t name,
			   uint32_t offset, uint32_t len,
			   uint32_t bytes_remaining);
};

/* ==========================================================================
 * 5. The port services (design SS3.2, "the three port services")
 *
 * Called from the cluster fork thread only.
 *
 * THE E9 BRIDGE (FC-P1.3, docs/cluster-integration-notes.md E9). The three
 * declarations below are the frozen glue-facing surface `struct vms_pe`'s
 * owner (FC-P0.9) implements; `body`/`len` is the SCS "body" contract E1/
 * SS3.2.4 grounds -- SCS's own abs 56-71 envelope followed by the 132-byte
 * SYSAP body (`PE_SEND_BODY_LEN`, vms_pe_fsm.h SS8c). The REAL, R1-tested
 * implementation is `pe_vc_send_msg`/`pe_vc_send_dg`/`pe_fsm_set_upper`
 * (vms_pe_fsm.h SS8c), pure `struct pe_fsm *` functions -- that is the
 * object `struct vms_pe` will embed once FC-P0.9 defines it (documented
 * "private to vms_pe.c" below), so the glue's job is exactly the one-line
 * dereference `pe_send_msg(pe, ...) { return pe_vc_send_msg(&pe->fsm, ...); }`,
 * never a second implementation of the sequencing/envelope logic.
 * ========================================================================== */

/* Send a sequenced message to `dst` addressed to the peer's `dst_conid`.
 * Returns 0, or an SS$_ status (no circuit, no credit, transmit failed). */
int pe_send_msg(struct vms_pe *pe, vms_scs_sysid_t dst, vms_conid_t dst_conid,
		const uint8_t *body, uint32_t len);

/* Send a datagram (unsequenced, unacknowledged) to `dst`. */
int pe_send_dg(struct vms_pe *pe, vms_scs_sysid_t dst,
	       const uint8_t *body, uint32_t len);

/* Register the upper layer (SCS). One registration per port; a second call
 * replaces it. */
void pe_set_upper(struct vms_pe *pe, const struct pe_upper_ops *upper);

/*
 * THE THIRD SERVICE -- BLOCK TRANSFER (FC-P6.1). Same E9 bridge shape as the
 * two above: these are the frozen glue-facing names, and the REAL, R1-tested
 * implementation is the pure `struct pe_fsm *` family in vms_pe_fsm.h SS8d
 * (pe_blk_buf_register / pe_blk_buf_release / pe_blk_send /
 * pe_blk_send_read_end / pe_blk_send_ack), so the glue's job is again the
 * one-line dereference and never a second implementation.
 *
 * `name_out` is the port's own token for the buffer; the SYSAP sends it to the
 * far SYSAP in an ordinary message (for MSCP, inside Table A-6's buffer
 * descriptor) and that is how the two ends correlate. Returns 0 or an SS$_
 * status -- the glue maps `enum pe_blk_status`, which is what the pure layer
 * actually holds (kernel-core cluster headers carry no SS$_ definitions,
 * design SS3.2.2).
 */
int pe_buf_register(struct vms_pe *pe, uint8_t *base, uint32_t len,
		    uint8_t access, uint32_t *name_out);
int pe_buf_release(struct vms_pe *pe, uint32_t name);

/* Move `x->length` bytes over the circuit to `x->peer`. See vms_pe_fsm.h SS8d
 * for the two framing forms (READ's end-message piggyback, WRITE's two-frame
 * request/response) and for why every remote-side field in `*x` is READ from
 * the peer's own message rather than chosen here. */
int pe_send_block(struct vms_pe *pe, const struct pe_blk_xfer *x,
		  uint32_t *frames_out);

/* ==========================================================================
 * 6. Lifecycle and readback (glue, vms_pe.c -- FC-P0.9)
 * ========================================================================== */

/* Bring the port up on the interface named in `cl->ifname`: open the LAN seam,
 * join the HELLO multicast group, create PEA0: in the device table and start the
 * HELLO cadence. Returns 0 or an SS$_ status -- SS$_NOSUCHDEV when the substrate
 * has no such interface, which is the honest end of the road: no PEA0:, no
 * HELLO, and CLUSTER_START fails with it (Rule 9). */
int vms_pe_start(struct vms_cluster *cl);

/* Stop the port: cancel timers, close the LAN seam, drop PEA0:. Idempotent. */
void vms_pe_stop(struct vms_cluster *cl);

/* Project the port's state. Both fill from REAL objects under the fork mutex
 * (INV-6); `index` walks channels / VCs and each returns 0, or SS$_NOSUCHDEV
 * past the end. */
int vms_pe_snapshot(struct vms_cluster *cl, struct vms_pe_view *out);
int vms_pe_channel_snapshot(struct vms_cluster *cl, uint32_t index,
			    struct vms_pe_channel_view *out);
int vms_pe_vc_snapshot(struct vms_cluster *cl, uint32_t index,
		       struct vms_pe_vc_view *out);

#endif /* OVMX_VMS_PE_H */
