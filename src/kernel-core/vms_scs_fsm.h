/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_scs_fsm.h - the PURE System Communication Services state machine:
 * the SB set, the CDL/CDT connection ladder, the Con.ID allocator, the
 * per-connection credit ledger and the MTYPE dispatch (plan item FC-P2.2).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.2 (the layer map),
 * SS3.2.4 (the body-level seam: SCS owns frame-absolute 56-71 and nothing
 * else), SS3.2.5 (THE VC-BREAK CONTRACT, implemented here), SS3.4 (SB/CDT
 * data model), SS3.9 (pure FSM + injected ops + a read-only view).
 * Wire spec: docs/cluster-protocol-spec.md SS4(h)(1b)/(1c)/(1f)/(1g) (the
 * MTYPE envelope, the credit field, the 8->9 exchange), SS4(m) (the op verb
 * set and the msgtype phase rule), SS4(t) (Con.ID allocation).
 * Published book: *VAXcluster Principles* (Davis 1993) ch. 2 -- the CDT/CDL,
 * the CONID pair, the Send/Receive/Pending credit triple, Credit Wait, the
 * SDIR/listening-CDT model, and the rule that all CDTs on a circuit are
 * queued to its Path Block so a break can walk them. Cited by page in the
 * .c file; the transcript is copyrighted and host-only.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS LAYER IS, IN ONE PARAGRAPH
 *
 * The port gives SCS a virtual circuit between two SYSTEMS. SCS turns that
 * into named CONNECTIONS between two SYSAPs. It keeps one SB per remote
 * system, one CDT per connection (queued to that SB), allocates the local
 * Con.ID, walks the connect verbs, keeps a debit/credit ledger per CDT, and
 * dispatches an arriving message by (destination Con.ID, MTYPE) into the
 * CDT's own input routine.
 *
 * ---------------------------------------------------------------------------
 * WHY THE LEDGER AND THE CDL ARE STRUCTURAL, NOT DECORATIVE
 *
 * In the strawman daemon the CDL delivery path and the credit accounting were
 * DEAD CODE: data went around them and the credit byte on the wire was a
 * template constant. Three structural rules make that impossible here.
 *
 *   1. THE ONLY WAY TO SEND IS scs_fsm_send_msg(), and it spends a credit off
 *      a real ledger. No credit, no send -- the message goes into CREDIT WAIT
 *      (ch. 2's own mechanism) or is refused; it is never sent anyway.
 *   2. THE ONLY WAY TO RECEIVE IS THE CDL. scs_fsm_rx_message() takes the
 *      destination Con.ID off the wire, indexes the CDL with its low 16 bits
 *      (ch. 2: "the low order 16 bits of the CONID ... are used as an index
 *      into the CDL to locate the CDT's address"), verifies the WHOLE Con.ID
 *      against the CDT, and only then reaches the SYSAP.
 *   3. EVERY CREDIT BYTE THIS FILE PUTS ON THE WIRE IS A LEDGER READ. The
 *      credit field of an outbound message is `cdt->credit_pending` -- the
 *      count of receive buffers the local SYSAP has actually released --
 *      and sending it RESETS that count, exactly as pp. 2-43..2-44 require.
 *      It is never a constant, never a copy of the inbound frame's field, and
 *      never a template (INV-6; the operator has caught frame-to-frame credit
 *      plumbing repeatedly, and a fabricated flow-control value is how a peer
 *      is driven into a state neither side can account for).
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS LAYER DOES NOT DO
 *
 *   - It does not sequence, acknowledge or retransmit. That is the port
 *     (design SS3.2.5): port-level retransmission is INVISIBLE here, and a
 *     message spends its credit exactly ONCE at scs_fsm_send_msg() however
 *     many times the port re-sends it.
 *   - It does not reconnect. On a VC break every CDT on that SB closes,
 *     path-lost, and stays closed. CNXMAN's recnx_fsm is the SYSAP that
 *     re-connects (spec SS4(aa)). That is why the ladder below needs no
 *     "suspended" state.
 *   - It does not write one byte outside frame-absolute 56-71 of an
 *     application message, and it builds every frame through the FC-P2.1
 *     codec -- there is not one raw wire offset in vms_scs_fsm.c
 *     (design SS3.9 rule 2).
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_SCS_FSM_H
#define OVMX_VMS_SCS_FSM_H

#include "vms_scs.h"
#include "vms_cluster_codec_scs.h"
#include "vms_cluster_codec_vc.h"

/* ==========================================================================
 * 1. Wire geometry this layer works in (all DERIVED from codec constants)
 * ========================================================================== */

/* The application-message class, spec SS4(d)/SS4(h)(1b): "the 190-content
 * class is uniformly type 10 with inner length 146". */
#define SCS_MSG_SCA_CONTENT  190u
#define SCS_MSG_FRAME_LEN    (VMS_ETH_HDR_LEN + SCS_MSG_SCA_CONTENT)   /* 204 */
#define SCS_INNERLEN_BIAS     44u   /* inner length == SCA content - 44   */
#define SCS_MSG_INNER_LEN    (SCS_MSG_SCA_CONTENT - SCS_INNERLEN_BIAS) /* 146 */

/* The body-level seam (design SS3.2.4): byte 0 of a "body" is abs 56. */
#define SCS_MSG_BODY_LEN     (SCS_MSG_FRAME_LEN - VMS_OFF_SCSCTRL_INNERLEN)  /* 148 */
#define SCS_SYSAP_BODY_LEN   (SCS_MSG_BODY_LEN - VMS_SCS_HDR_LEN)            /* 132 */

/* The largest connection-control frame (op 0/2, the 110-content class). */
#define SCS_CTRL_FRAME_MAX   (VMS_ETH_HDR_LEN + VMS_SCSCTRL_LEN_CONNECT)     /* 124 */

/*
 * THE SECOND GROUNDED APPLICATION-MESSAGE CLASS (FC-P2.3).
 *
 * MTYPE 10 is not one length. Spec SS4(h)(1b) states the envelope is uniform
 * across "the short classes here, the 94-content MSCP commands, and the
 * 190-content class", and SS4(h)(2)/(2a) grounds the directory message as
 * MTYPE 10 on the 94-content class -- the reference capture's twelve directory
 * messages are "all of length 94". So a SYSAP body is 132 bytes on the
 * 190-content class and 36 on the 94-content one, and scs_fsm_send_msg()
 * selects the class FROM THE LENGTH the SYSAP handed down (the two grounded
 * values, nothing between them).
 *
 * The 94-content class does NOT go through the body-level SCS<->port seam:
 * pe_vc_send_msg requires exactly PE_SEND_BODY_LEN (148), so -- exactly as
 * for the connect verbs (E1's FC-P2.2 addendum) -- SCS builds the whole frame
 * through the codec and hands it to the port's frame-level primitive.
 */
#define SCS_DIR_FRAME_LEN    (VMS_ETH_HDR_LEN + VMS_SCSCTRL_LEN_LOOKUP)      /* 108 */
#define SCS_DIR_BODY_LEN     VMS_SCS_DIRBODY_LEN                             /*  36 */

/*
 * THE THIRD APPLICATION-MESSAGE SHAPE (FC-P6.3).
 *
 * MTYPE 10 is not two lengths either. Spec sec 4(h)(1b) states the envelope is
 * UNIFORM "across the short classes here, the 94-content MSCP commands, and the
 * 190-content class" -- a statement about the ENVELOPE, not about a closed set
 * of lengths -- and FC-P6.2's own census MEASURES the MSCP server's five end
 * messages at five SCA contents: 86 (SCC END, 954/954 frames), 90 (READ END),
 * 94 (WRITE END), 102 (ONLINE END) and 110 (GUS END, 18855/18855). Two of those
 * are the two classes above; three are not, and a server that cannot emit them
 * cannot answer a MOUNT.
 *
 * So scs_fsm_send_msg() takes a SYSAP body of ANY length up to
 * SCS_SYSAP_BODY_LEN: 132 and 36 keep their own grounded, byte-exact builders
 * (the 190-content class and the 94-content one), and every other length is
 * assembled from this layer's own 16-byte header plus the SYSAP's bytes and
 * handed to the port's variable-length entry, with the SCA length field made to
 * match the frame that really went out. Nothing is padded into a class it does
 * not belong to, and nothing is truncated.
 *
 * The two derivations below are the arithmetic that makes an SCS header for
 * such a body. Both are DERIVED from already-published constants -- the codec's
 * own SYSAP-body origin (frame-absolute 72) and Ethernet header length, and
 * SCS_INNERLEN_BIAS above -- so the 190/94 classes fall out of them rather than
 * being restated: 132 -> content 190, inner 146.
 */
#define SCS_SYSAP_SCA_CONTENT(sysap_len) \
	((uint16_t)((VMS_OFF_SYSAP_BODY - VMS_ETH_HDR_LEN) + (sysap_len)))
#define SCS_SYSAP_INNER_LEN(sysap_len) \
	((uint16_t)(SCS_SYSAP_SCA_CONTENT(sysap_len) - SCS_INNERLEN_BIAS))

/* ==========================================================================
 * 2. Return statuses -- negative, so `if (rc)` reads as failure
 *
 * The pure FSM holds NO SS$_ definitions (design SS3.2.2); the glue maps each
 * of these onto the SS$_ status vms_scs.h's own entry points promise. The
 * mapping is 1:1 and named beside each value.
 * ========================================================================== */
enum scs_fsm_status {
	SCS_OK            =  0,
	SCS_ERR_INVAL     = -1,  /* a NULL/otherwise impossible argument      */
	SCS_ERR_NOCONN    = -2,  /* no CDT with that Con.ID  -> SS$_NOSUCHID  */
	SCS_ERR_NOTOPEN   = -3,  /* the CDT is not OPEN      -> SS$_INCONSTATE*/
	SCS_ERR_NOCREDIT  = -4,  /* Send Credit 0, no Credit Wait -> SS$_EXQUOTA */
	SCS_ERR_NOCDT     = -5,  /* the CDL is exhausted     -> SS$_INSFMEM   */
	SCS_ERR_NOSB      = -6,  /* the SB table is full     -> SS$_INSFMEM   */
	SCS_ERR_NOCONID   = -7,  /* the allocator is unseeded (see SS4)       */
	SCS_ERR_NOPATH    = -8,  /* no VC to that system     -> SS$_NOSUCHNODE*/
	SCS_ERR_PATHLOST  = -9,  /* the circuit was lost     -> SS$_PATHLOST  */
	SCS_ERR_TXFAIL    = -10, /* ops->send_* refused the frame             */
	SCS_ERR_NOSYSAP   = -11, /* no SYSAP listening on that name           */
	SCS_ERR_BUSY      = -12, /* the listening CDT already has a connect   */
	SCS_ERR_ADDR      = -13, /* ops->addr could not supply REAL addressing */
	SCS_ERR_CODEC     = -14  /* the codec refused to build/parse          */
};

/*
 * Why a CDT closed. Handed to scs_sysap_ops.closed(); the glue maps it to the
 * SS$_ status a user-mode reader sees (SCS_CLOSE_PATHLOST -> SS$_PATHLOST).
 */
enum scs_close_reason {
	SCS_CLOSE_NONE       = 0,
	SCS_CLOSE_LOCAL      = 1,  /* our own DISCONNECT completed            */
	SCS_CLOSE_REMOTE     = 2,  /* the peer's DISCONNECT completed         */
	SCS_CLOSE_REJECTED   = 3,  /* the peer answered our connect with op 4 */
	SCS_CLOSE_TIMEOUT    = 4,  /* a connect/disconnect verb went unanswered */
	SCS_CLOSE_PATHLOST   = 5,  /* the virtual circuit was lost (SS3.2.5)  */
	SCS_CLOSE_UNLISTEN   = 6,  /* the local SYSAP withdrew its name       */
	SCS_CLOSE_REASON__COUNT
};

const char *scs_cdt_state_name(enum vms_scs_cdt_state s);
const char *scs_close_reason_name(enum scs_close_reason r);
const char *scs_mtype_name(enum scs_mtype m);

/* ==========================================================================
 * 3. The injected ops -- the FSM's ONLY route to the world
 *
 * `send_ctrl` and `send_msg` are TWO entries because the wire has two shapes,
 * not because there are two designs (see vms_cluster_codec_scs.h, "the
 * body-level SCS header"): the connect verbs occupy short SCA classes that
 * only SCS can build whole, while an application message is the fixed
 * 190-content class whose bytes three layers own. In production they are
 * pe_vc_send_frame and pe_vc_send_msg respectively.
 * ========================================================================== */
struct scs_fsm_ops {
	/*
	 * Transmit a COMPLETE connection-control frame (ops 0-9), already built
	 * through vms_scs_ctrl_build(). The port stamps abs 32-35 (+ mirror)
	 * and owns retransmission. Production: pe_vc_send_frame. Return 0, or
	 * non-zero to refuse (nothing was sent).
	 */
	int (*send_ctrl)(void *ctx, vms_scs_sysid_t dst,
			 const uint8_t *frame, uint32_t len);

	/*
	 * Transmit an application message BODY: exactly SCS_MSG_BODY_LEN bytes,
	 * frame-absolute 56 onward -- this layer's own 16-byte header followed
	 * by the SYSAP's 132. Production: pe_vc_send_msg.
	 */
	int (*send_msg)(void *ctx, vms_scs_sysid_t dst, vms_conid_t dst_conid,
			const uint8_t *body, uint32_t len);

	/*
	 * The SAME service at a body length the SYSAP chooses (ADDED BY
	 * FC-P6.3). Production: pe_vc_send_msg_var. See SS1's "THE THIRD
	 * APPLICATION-MESSAGE SHAPE" note for what grounds it -- five MEASURED
	 * MSCP end-message classes that are neither 190-content nor 94-content.
	 * MAY BE NULL: a glue that has not bound it simply cannot carry those
	 * classes, and scs_fsm_send_msg() then refuses honestly (SCS_ERR_TXFAIL)
	 * instead of padding a body into a class it does not belong to.
	 */
	int (*send_msg_var)(void *ctx, vms_scs_sysid_t dst,
			    vms_conid_t dst_conid, const uint8_t *body,
			    uint32_t len);

	/*
	 * The four REAL addresses of the circuit to `dst`, read from the
	 * channel that circuit rides (production: pe_vc_addr). SCS INVENTS NO
	 * ADDRESSING: if this returns non-zero, no frame is built and no frame
	 * is sent -- an honest refusal, counted, rather than a frame carrying a
	 * guessed MAC or a mirrored logical address (spec SS4(a).0; mirroring
	 * abs 0 into abs 16 makes a real peer drop every reply).
	 */
	int (*addr)(void *ctx, vms_scs_sysid_t dst, struct vms_scs_addr *out);

	void (*arm_timer)(void *ctx, enum scs_timer which, uint32_t key, uint32_t ms);
	void (*cancel_timer)(void *ctx, enum scs_timer which, uint32_t key);

	uint32_t (*now_ms)(void *ctx);
	void     (*log)(void *ctx, const char *msg);

	void *ctx;
};

/* ==========================================================================
 * 4. The Con.ID allocator (spec SS4(t) + ch. 2's CDL rule)
 *
 * A Con.ID is (uniquifier << 16) | (CDL index + 1). Two published/grounded
 * facts fix that shape and one OVMX choice fills the gap between them:
 *
 *   - PUBLISHED (ch. 2): "The low order 16 bits of the CONID associated with a
 *     CDT are used as an index into the CDL to locate the CDT's address."
 *     That is why the low half is an index and not a free-running counter,
 *     and why receive-side dispatch is an index plus a full-value check.
 *   - GROUNDED (spec SS4(t)): a real node's Con.IDs across one boot were
 *     0x33590007 / 0x33580008 / 0x33580009 -- low halves marching 7, 8, 9
 *     across DIFFERENT service classes (one shared allocation order), high
 *     half re-seeding NON-ARITHMETICALLY at each incarnation, so "a real node
 *     never repeats a Con.ID across incarnations".
 *   - OVMX'S OWN CHOICE, labelled as one (Rule 8 -- VMS's seed function is
 *     unpublished and is NOT re-derived here): the uniquifier is
 *     `boot_seed + slot_generation`, where `boot_seed` is supplied by the glue
 *     from a LIVE per-boot value (the incarnation-time quadword the port
 *     already holds) and `slot_generation` counts how many times that CDL slot
 *     has been reused. That satisfies both grounded properties -- never a
 *     repeat within a boot (generation moves), never a repeat across boots
 *     (the seed moves) -- without guessing at VMS's arithmetic.
 *
 * +1 ON THE INDEX because the WIRE uses Con.ID 0 for "not bound yet"
 * (spec SS4(m): a CONNECT-REQUEST carries remote_conid = 0). A Con.ID whose
 * low half is 0 would be indistinguishable from that, so it is never minted.
 *
 * THE ALLOCATOR REFUSES UNTIL IT IS SEEDED. scs_fsm_seed_conid() must be
 * called with a real per-boot value before any connection can be made; an
 * unseeded allocator returns SCS_ERR_NOCONID rather than minting from 0.
 * Minting from a fixed 0 would produce the exact repeat-across-incarnations
 * SS4(t) says a real node cannot produce, and a placeholder connection
 * identifier is the class of fabrication that bugchecked a real VAX (INV-6).
 * ========================================================================== */
struct scs_conid_alloc {
	uint16_t seed;      /* the per-boot value the glue supplied     */
	uint8_t  seeded;    /* 0 = refuse to allocate (see above)       */
	uint8_t  pad0;
	uint32_t next_slot; /* round-robin start for the CDL scan       */
	uint32_t minted;    /* how many Con.IDs this boot has produced  */
};

/* ==========================================================================
 * 5. The objects
 * ========================================================================== */

/*
 * SB -- one per remote SYSTEM (design SS3.4). It also plays ch. 2's Path
 * Block role for the CDT queue: "SCA specifies that all CDTs corresponding to
 * connections supported by a virtual circuit be queued to the Path Block
 * corresponding to that circuit. If the circuit is broken ... it is then a
 * relatively simple matter to scan this queue to determine which connections
 * have also been lost." `cdt_head` IS that queue, and scs_fsm_vc_down() is
 * that scan.
 */
struct scs_sb {
	uint8_t          in_use;
	uint8_t          vc_up;         /* the port has an OPEN circuit here  */
	uint8_t          pad0[2];
	vms_scs_sysid_t  peer_sysid;
	uint32_t         cdt_head;      /* head of the CDT queue, SCS_NIL end */
	uint32_t         n_cdts;        /* CDTs currently on the queue        */
	uint32_t         vc_ups;        /* circuits formed to this system     */
	uint32_t         vc_downs;      /* ... and lost                       */
	uint32_t         cdts_pathlost; /* connections killed by a break      */
};

#define SCS_NIL 0xffffffffu   /* the end of an intrusive index list */

/*
 * CDT -- one per connection, plus (ch. 2) one LISTENING CDT per registered
 * SYSAP name. A listening CDT has no peer and no ledger; it exists so an
 * inbound CONNECT_REQ has somewhere to be recorded while the SYSAP decides,
 * and it is what the frozen VMS_SCS_CDT_LISTEN / _CONNECT_RCVD states name.
 */
struct scs_cdt {
	uint8_t   in_use;
	uint8_t   state;              /* enum vms_scs_cdt_state              */
	uint8_t   is_listening;       /* ch. 2's listening CDT               */
	uint8_t   initiator;          /* WE sent the CONNECT-REQUEST         */

	uint8_t   remote_conid_valid; /* 0 = never learned; NOT "Con.ID 0"   */
	uint8_t   echo_rcvd;          /* the op-1 CONNECT-ECHO arrived       */
	uint8_t   confirmed;          /* our op-3 CONFIRM was transmitted    */
	uint8_t   close_reason;       /* enum scs_close_reason               */

	/* ---- the teardown half-exchanges (spec SS4(m): "each side sends its
	 * own op 6 and answers the peer's with op 7") ---- */
	uint8_t   disc_sent;          /* our op 6 is on the wire             */
	uint8_t   disc_matched;       /* ... and the peer answered it (op 7) */
	uint8_t   disc_peer_matched;  /* the peer's op 6 got our op 7        */
	uint8_t   disc_pending;       /* a local DISCONNECT is being walked  */

	/* ---- the special credit message, spec SS4(h)(1f)/(1g) ---- */
	uint8_t   credit_msg_sent;    /* an op 8 is outstanding              */
	uint8_t   credit_msg_done;    /* the 8->9 exchange completed         */
	/*
	 * Spec SS4(m)'s msgtype phase rule, which is SCS KNOWLEDGE passed down
	 * to the port, never a byte a SYSAP sets: a connection's frames carry
	 * 0x5b while it is being established -- "the joiner's own CONNECT-
	 * REQUESTs ... its op 3 confirms and its FIRST directory lookups" --
	 * and 0x4b once it is in data phase ("LATER lookups on an established
	 * directory connection"). So the flag turns over on the first
	 * application message this CDT actually transmits, not on reaching
	 * OPEN. Getting it wrong is the SIGNATURE failure: the member echoes
	 * (op 1) and never accepts (op 2).
	 */
	uint8_t   data_phase;
	uint8_t   pad0;

	/*
	 * The slot's REUSE GENERATION -- the only field of this struct that
	 * SURVIVES release, because it is what makes the next Con.ID minted in
	 * this CDL slot differ from the last one (vms_scs_fsm.h SS4).
	 */
	uint16_t  generation;
	uint16_t  pad2;

	uint32_t  sb_index;           /* the SB (Path Block) this rides      */
	uint32_t  sb_next;            /* next CDT on that SB's queue         */
	uint32_t  listen_index;       /* for a connection CDT: the listening
				       * CDT it was accepted from, else SCS_NIL */

	vms_conid_t local_conid;      /* what OUR allocator minted           */
	vms_conid_t remote_conid;     /* what the PEER minted, when learned  */
	vms_scs_sysid_t peer_sysid;

	uint8_t   local_name[VMS_SCS_PROCNAME_LEN];
	uint8_t   remote_name[VMS_SCS_PROCNAME_LEN];
	uint8_t   conndata[VMS_SCS_PROCNAME_LEN];  /* spec SS4(N), verbatim  */

	const struct scs_sysap_ops *sysap;

	/*
	 * ---- THE CREDIT LEDGER (pp. 2-43..2-44; spec SS4(h)(1c)/(1g)) ----
	 *
	 * grant   -- how many receive buffers WE extended at connect time.
	 *            The conservation invariant below is stated against it.
	 * send    -- Send Credit: messages we may send. Created ONLY by credit
	 *            the peer carried; if the peer's connect carried 0, this
	 *            stays 0 and every send is refused. No window is invented.
	 * receive -- Receive Credit: the mirror of the peer's Send Credit, i.e.
	 *            buffers still extended and not yet consumed.
	 * held    -- arrived and NOT yet released by the local SYSAP.
	 * pending -- Pending Receive Credit: released by the SYSAP, not yet
	 *            told to the peer. This is the value that goes on the wire.
	 *
	 * CONSERVATION (asserted by the R1 property test, and the reason `held`
	 * is a real field rather than a derivation):
	 *
	 *     receive + held + pending == grant        at EVERY point
	 *
	 * and, at quiescence, our `send` equals the peer's `receive`.
	 *
	 * WHICH MESSAGE TYPES MOVE WHICH COUNTER is spelled out at
	 * credit_carry_inbound() in the .c, including the one place OVMX had
	 * to choose a reading (types 8 and 9 carry credit but spend none) and
	 * why p. 2-44's own statement of what the special credit message is
	 * FOR decides it.
	 */
	uint16_t  credit_grant;
	uint16_t  credit_send;
	uint16_t  credit_receive;
	uint16_t  credit_held;
	uint16_t  credit_pending;

	/*
	 * ---- MINIMUM SEND CREDITS, both ends (p. 2-44) ----
	 *
	 * `local_min_send_credits` is what THIS end's SYSAP passed to its own
	 * CONNECT/ACCEPT service (scs_connect_args.min_credits /
	 * scs_fsm_accept()'s argument). It is the value SCS$W_MIN_CR carries on
	 * the verbs this CDT emits. 0 is a real answer -- "this SYSAP declares
	 * no floor" -- not a placeholder, and it is what every SYSAP OVMX
	 * registers today genuinely requires.
	 *
	 * `peer_min_send_credits` is the SAME argument as the REMOTE SYSAP
	 * passed it, read off the CONNECT_REQ/ACCEPT_REQ this CDT answered or
	 * received (integration note E65 grounds SCS$W_MIN_CR at abs 72-73;
	 * before that the offset was unknown and this stayed unset). p. 2-44's
	 * rule -- the local Receive Credit is "dangerously low" below
	 * SCSFLOWCUSH + this -- therefore runs on the peer's real number.
	 * `_valid` is still the honest-omission flag: a connection whose
	 * connect frame did not parse never learns it, the trigger falls back
	 * to the cushion alone, and that fallback is counted in
	 * scs_fsm.credit_msg_partial_threshold.
	 */
	uint16_t  local_min_send_credits;
	uint16_t  peer_min_send_credits;
	uint8_t   peer_min_send_credits_valid;
	uint8_t   pad1[3];

	/* ---- Credit Wait (p. 2-45): the FIFO of sends held for credit ---- */
	uint32_t  sw_head;
	uint32_t  sw_tail;
	uint32_t  sw_count;

	/* ---- counters, all real events ---- */
	uint32_t  msgs_sent;
	uint32_t  msgs_received;
	uint32_t  credit_stalls;         /* sends that entered Credit Wait   */
	uint32_t  credit_overruns;       /* peer sent past its Send Credit   */
	uint32_t  sends_failed_pathlost; /* Credit Wait killed by a break    */
	uint32_t  timer_key;             /* == this CDT's CDL index          */

	/*
	 * ---- THE LAST REFUSAL, VERBATIM (integration note E70) ----
	 *
	 * WHY THE EXECUTIVE HAS TO KEEP THESE. A caller of scs_send_msg() is
	 * told an SS$_ status, and that mapping is deliberately MANY-TO-ONE:
	 * vms_scs.c cannot cite OpenVMS's SS$_INCONSTATE / SS$_PATHLOST /
	 * SS$_NOSUCHNODE values and Rule 8 forbids inventing them, so
	 * NOTOPEN, NOPATH and PATHLOST all answer SS$_DEVOFFLINE and every
	 * transport refusal answers SS$_ABORT. On a live cluster that is the
	 * difference between "the connection was not sendable", "the port's
	 * send window is spent" and "the interface refused the frame" -- three
	 * different defects with three different fixes -- collapsed into one
	 * number (E70: three promotion messages were refused on a live VAX
	 * cluster and nothing recorded which of the five possible refusals
	 * fired).
	 *
	 * So the CDT keeps what the executive really produced:
	 *
	 *   tx_last_err      this FSM's own `enum scs_err` for the most recent
	 *                    REFUSED send on this connection. 0 (SCS_OK) means
	 *                    no send on this CDT has ever been refused.
	 *   tx_last_port_rc  the INJECTED send op's own refusal code, verbatim
	 *                    and uninterpreted, when tx_last_err is
	 *                    SCS_ERR_TXFAIL -- i.e. when the refusal came from
	 *                    below this layer. 0 whenever the port was not the
	 *                    refuser, which is an honest "not applicable" and
	 *                    not a success code.
	 *   tx_refusals      how many sends this connection has refused.
	 *
	 * These are DIAGNOSTIC state, read back through
	 * scs_fsm_send_refusal(). Nothing in this file branches on them and no
	 * byte of them reaches the wire.
	 */
	int32_t   tx_last_err;
	int32_t   tx_last_port_rc;
	uint32_t  tx_refusals;
};

/* One queued send in Credit Wait. The pool is BOUND by the glue, so the FSM
 * still allocates nothing; with no pool bound a creditless send is REFUSED
 * (SCS_ERR_NOCREDIT) instead of queued, which is honest and counted.
 * `len` is the SYSAP's own length, so a queued directory message resumes as a
 * 94-content frame and a queued CNXMAN body as a 190-content one -- the class
 * is decided by what the SYSAP handed down, never by where it was queued. */
struct scs_sendwait {
	uint8_t  in_use;
	uint8_t  pad0[3];
	uint32_t next;
	uint32_t cdt_index;
	uint32_t len;
	uint8_t  body[SCS_SYSAP_BODY_LEN];
};

/* An SDIR (ch. 2, "SCS Directory Entries and Listening CDTs"): a registered
 * SYSAP name and the listening CDT that carries its connect-request handler. */
#define SCS_MAX_SYSAPS 8u

struct scs_sdir {
	uint8_t  in_use;
	uint8_t  pad0[2];
	/*
	 * FC-P2.3: the 16 bytes THIS SYSAP wants a directory HIT on its name to
	 * carry (spec SS4(h)(2) RE gap (c) -- an affirmative result is an
	 * opaque per-connection descriptor whose internal semantics are NOT
	 * grounded). It is supplied by the SYSAP that owns the name, through
	 * scs_fsm_sysap_set_dir_data(); the directory NEVER invents one, and
	 * with none supplied it answers with the registered name itself (see
	 * vms_scs_dir.h "what a HIT carries"). `_valid` is the honest-omission
	 * flag: 0 means this SYSAP declared nothing, not "sixteen zeros".
	 */
	uint8_t  dir_data_valid;
	uint8_t  dir_data[VMS_SCS_PROCNAME_LEN];
	uint8_t  name[VMS_SCS_PROCNAME_LEN];
	uint16_t initial_credits;   /* what this SYSAP extends per connection */
	uint16_t pad1;
	uint32_t listen_cdt;        /* index into the CDL                     */
	const struct scs_sysap_ops *ops;
};

/*
 * Tunables. Both timeouts are OVMX DESIGN VALUES, labelled as such per the
 * spec's SS5 discipline -- no capture measures an SCS verb timeout. What IS
 * grounded bounds them: a member's connect-back is sub-2 s in every reference
 * specimen (SS4(v)), and a teardown is machine-speed (SS4(h)(1f): 8->9 worst
 * case 3.1 ms, 9->op-6 worst case 2.1 ms). The real backstop underneath both
 * is the port's TIMVCFAIL, which takes the whole circuit down.
 *
 * `flowcush` is SYSGEN SCSFLOWCUSH, whose PUBLISHED VAX/VMS V7.3 default is 1
 * (min 0, max 16, dynamic) -- that is a documented parameter default, not a
 * value inferred from a capture, and at that setting a real VAX emitted ZERO
 * special credit messages in 440 367 frames (spec SS4(h)(1g)), which is the
 * behaviour this default reproduces.
 */
#define SCS_CONNECT_TIMEOUT_MS_DEFAULT    10000u
#define SCS_DISCONNECT_TIMEOUT_MS_DEFAULT  5000u
#define SCS_FLOWCUSH_DEFAULT                  1u

struct scs_fsm_cfg {
	uint32_t connect_timeout_ms;
	uint32_t disconnect_timeout_ms;
	uint16_t flowcush;
	uint16_t pad0;
};

struct scs_fsm {
	const struct scs_fsm_ops *ops;
	struct scs_fsm_cfg        cfg;
	struct scs_conid_alloc    conid;

	/* ---- bound tables (the glue sizes them; the FSM allocates nothing) */
	struct scs_cdt      *cdl;       /* the Connection Descriptor List     */
	uint32_t             n_cdl;
	struct scs_sb       *sbs;
	uint32_t             n_sbs;
	struct scs_sendwait *sw;        /* the Credit Wait pool, may be NULL  */
	uint32_t             n_sw;

	struct scs_sdir      sdir[SCS_MAX_SYSAPS];

	/* ---- port-wide counters, every one a real event ---- */
	uint32_t ignored_events;        /* [cdt state][event] with no edge    */
	uint32_t rx_frames;             /* frames handed to scs_fsm_rx_*      */
	uint32_t rx_parse_failed;       /* the codec refused to decode one    */
	uint32_t rx_no_cdt;             /* a Con.ID that indexes no live CDT  */
	uint32_t rx_conid_mismatch;     /* right slot, WRONG uniquifier       */
	uint32_t rx_undelivered;        /* accounted, no SYSAP took it        */
	uint32_t tx_refused_addr;       /* ops->addr had no REAL addressing   */
	uint32_t tx_refused_codec;      /* the codec refused to build         */
	uint32_t tx_errors;             /* ops->send_* refused the frame      */
	uint32_t connects_rejected;     /* op 4 we SENT: no SYSAP, or busy    */
	uint32_t connect_no_sysap;
	uint32_t connect_busy;
	uint32_t credit_msgs_sent;      /* op 8 we originated                 */
	uint32_t credit_msgs_answered;  /* op 9 that matched one              */
	uint32_t credit_msg_partial_threshold; /* fired without the peer's
						* Minimum Send Credits (above)  */
	uint32_t disc_without_credit_msg; /* teardown that could not send op 8 */
	uint32_t dir_lookups_served;
	uint32_t dir_lookups_sent;
	uint32_t credit_stalls;         /* sends that entered Credit Wait     */

	/* Scratch for ONE frame. Sized for the largest control class; an
	 * application message is built into `msgbuf`. Neither is ever on the
	 * stack (the VAX kernel stack is small). */
	uint8_t  ctrlbuf[SCS_CTRL_FRAME_MAX];
	uint8_t  msgbuf[SCS_MSG_BODY_LEN];
};

/* ==========================================================================
 * 6. Lifecycle and binding
 * ========================================================================== */

/* Zero the context and bind the ops. Arms no timer, allocates nothing,
 * seeds no Con.ID. Returns SCS_OK or SCS_ERR_INVAL. */
int scs_fsm_init(struct scs_fsm *f, const struct scs_fsm_ops *ops);

/* Bind the CDL (ch. 2: SCSCONNCNT entries plus the 200 spares, sized by the
 * glue), the SB table, and -- optionally -- the Credit Wait pool. Each takes
 * zeroed storage the caller owns. Call AFTER scs_fsm_init. */
int scs_fsm_bind_cdl(struct scs_fsm *f, struct scs_cdt *cdl, uint32_t n);
int scs_fsm_bind_sbs(struct scs_fsm *f, struct scs_sb *sbs, uint32_t n);
int scs_fsm_bind_sendwait(struct scs_fsm *f, struct scs_sendwait *sw, uint32_t n);

/* Seed the Con.ID allocator from a LIVE per-boot value (SS4). Until this is
 * called every allocation returns SCS_ERR_NOCONID. */
void scs_fsm_seed_conid(struct scs_fsm *f, uint16_t boot_seed);

/* Overwrite the tunables. `cfg` NULL restores the documented defaults. */
void scs_fsm_set_cfg(struct scs_fsm *f, const struct scs_fsm_cfg *cfg);

/* Tear every connection down (CLUSTER_STOP): each SYSAP is told, nothing is
 * put on the wire -- a shutdown is not a dialogue. */
void scs_fsm_stop(struct scs_fsm *f);

/* ==========================================================================
 * 7. THE SYSAP REGISTRY -- ch. 2's "list of listening SYSAPs"
 *
 * FC-P2.2 landed the SDIR queue itself: LISTEN allocates an SDIR carrying the
 * SYSAP's name plus a listening CDT holding its connect-request routine, and
 * an inbound CONNECT_REQ is routed by scanning that queue for a matching name
 * (p. 2-48). FC-P2.3 GROWS THAT ONE TABLE -- it does not add a second:
 *
 *   - scs_fsm_sysap_lookup() is the READ the SCS Directory Service answers
 *     from. p. 2-50: SCA "requires that there be an SCS Directory Service on
 *     each node that answers 'Yes' or 'No' when asked if a particular SYSAP
 *     name is present in its list of listening SYSAPs" -- so a HIT is a hit on
 *     THIS table and nothing else. There is no directory-specific name store
 *     to drift out of step with what is really listening (INV-6: an answer
 *     that outlives the registration it describes is a fabricated one).
 *   - scs_fsm_sysap_set_dir_data() lets the SYSAP that OWNS a name declare
 *     what an affirmative answer about it carries (struct scs_sdir).
 *
 * The vms_scs.h names (scs_sysap_listen/_connect/_accept/_send_msg/
 * _return_credit ...) are the GLUE-level spelling of the same services on
 * `struct vms_scs`, which is FC-P2.4's object: each is a one-line dereference
 * into its scs_fsm_* twin here, exactly as vms_pe.h's pe_send_msg wraps
 * pe_vc_send_msg (integration note E9). They are NOT defined in this file
 * because `struct vms_scs` is undefined outside vms_scs.c.
 * ========================================================================== */
int scs_fsm_listen(struct scs_fsm *f, const uint8_t *name,
		   const struct scs_sysap_ops *ops, uint16_t initial_credits);
int scs_fsm_unlisten(struct scs_fsm *f, const uint8_t *name);

/*
 * What the registry knows about ONE registered name. Every field is a copy of
 * live SDIR state; nothing is derived and nothing is defaulted.
 */
struct scs_sysap_info {
	uint8_t     name[VMS_SCS_PROCNAME_LEN];
	uint8_t     dir_data_valid;   /* 0 = this SYSAP declared none        */
	uint8_t     pad0[3];
	uint8_t     dir_data[VMS_SCS_PROCNAME_LEN];
	uint16_t    initial_credits;
	uint16_t    pad1;
	vms_conid_t listen_conid;     /* the listening CDT's Con.ID (p. 2-48) */
};

/*
 * Is `name` in this node's list of listening SYSAPs? SCS_OK and *out filled
 * for a registration that EXISTS RIGHT NOW; SCS_ERR_NOSYSAP otherwise, with
 * *out untouched. `out` may be NULL when only the yes/no is wanted.
 */
int scs_fsm_sysap_lookup(const struct scs_fsm *f, const uint8_t *name,
			 struct scs_sysap_info *out);

/*
 * The ops table `name` was registered with, or NULL (FC-P2.4). The SAME SDIR
 * queue scs_fsm_sysap_lookup reads: vms_scs.h's frozen CONNECT service names
 * the local SYSAP by NAME and carries no callback table, so the glue reads the
 * SYSAP's own from the ONE registry instead of keeping a second name table
 * beside it (integration note E20). Deliberately NOT a field of
 * struct scs_sysap_info, which is a readback VIEW.
 */
const struct scs_sysap_ops *scs_fsm_sysap_ops(const struct scs_fsm *f,
					      const uint8_t *name);

/*
 * Declare the 16 bytes an affirmative directory answer about `name` carries.
 * `data` NULL CLEARS the declaration (back to honest omission). Only the SYSAP
 * that registered the name has any business calling this.
 */
int scs_fsm_sysap_set_dir_data(struct scs_fsm *f, const uint8_t *name,
			       const uint8_t *data);

/* ==========================================================================
 * 8. Connection and data services
 * ========================================================================== */

/*
 * The arguments of the CONNECT service, as one struct rather than six
 * positional parameters.
 *
 * `initial_credits` is how many receive buffers THIS end extends to the peer
 * -- p. 2-43's "the local SYSAP is said to have extended N Send Credits to the
 * remote SYSAP" -- and it is the value that goes into the op-0 credit field
 * and seeds this CDT's whole ledger. A SYSAP that extends 0 can never be sent
 * to; that is a real configuration, not an error.
 *
 * `min_credits` is the OTHER credit argument p. 2-44 gives the CONNECT service:
 * this SYSAP's MINIMUM SEND CREDITS, the floor below which the peer must treat
 * its Receive Credit as dangerously low. It is what SCS$W_MIN_CR carries on
 * this connection's op 0 (integration note E65). 0 means "this SYSAP declares
 * no floor" -- a real answer, and the one every SYSAP OVMX registers today
 * genuinely gives, since none of them reserve a credit for anything.
 *
 * `conndata` is spec SS4(N)'s 16-byte SCA connect data (p. 2-25), carried
 * through UNINTERPRETED -- the Connection Managers' version handshake lives in
 * it, and deciding what it says is FC-P3.x's business, never SCS's. NULL means
 * this SYSAP supplied none, and the field goes out BLANK-FILLED (0x20), which
 * is what a real node puts there on a no-connect-data connect (E65 census:
 * 100% of no-data connects); it is an honest "nothing supplied", not a value
 * invented on the SYSAP's behalf.
 */
struct scs_connect_args {
	const uint8_t              *local_name;   /* VMS_SCS_PROCNAME_LEN     */
	const uint8_t              *remote_name;  /* VMS_SCS_PROCNAME_LEN     */
	const uint8_t              *conndata;     /* 16 bytes, or NULL        */
	const struct scs_sysap_ops *sysap;
	vms_scs_sysid_t             dst;
	uint16_t                    initial_credits;
	uint16_t                    min_credits;  /* p. 2-44, SCS$W_MIN_CR    */
};

/* Open a connection. Allocates a CDT (ch. 2: "VMS optimistically assumes that
 * a connect request will probably be accepted, and thus allocates a CDT as
 * part of ... the CONNECT service"), mints its Con.ID, emits op 0 and returns
 * the Con.ID in *out_conid. On any refusal the CDT is released and nothing
 * went on the wire. */
int scs_fsm_connect(struct scs_fsm *f, const struct scs_connect_args *a,
		    vms_conid_t *out_conid);

/* Answer an inbound connect. `listen_conid` is the value connect_req was
 * handed. Accept allocates the connection's OWN CDT and returns its Con.ID
 * (ch. 2), and may carry its own 16-byte SS4(N) connect data; reject allocates
 * no CDT at all (ch. 2: "The SCS REJECT service does not involve a CDT").
 * `min_credits` is the accepting SYSAP's own Minimum Send Credits (p. 2-44),
 * exactly as scs_connect_args.min_credits is the connecting SYSAP's: it is
 * what SCS$W_MIN_CR carries on the op-2 ACCEPT_REQ this call emits. */
int scs_fsm_accept(struct scs_fsm *f, vms_conid_t listen_conid,
		   const uint8_t *conndata, uint16_t min_credits,
		   vms_conid_t *out_conid);
int scs_fsm_reject(struct scs_fsm *f, vms_conid_t listen_conid);

/* Disconnect an OPEN connection. Walks the GROUNDED teardown: the special
 * credit message (op 8) first, its op-9 answer, then DISCONNECT-REQUEST --
 * spec SS4(h)(1f), 131 of 131 dialogues, "always the last thing on the
 * connection before DISCONNECT_REQ". */
int scs_fsm_disconnect(struct scs_fsm *f, vms_conid_t local_conid);

/* Send one application message. `body` is the SYSAP's OWN bytes, with not one
 * byte of any lower header in it, and `len` is one of the two GROUNDED
 * application-message classes -- SCS_SYSAP_BODY_LEN (132, the 190-content
 * class) or SCS_DIR_BODY_LEN (36, the 94-content directory class, SS4(h)(2)).
 * Any other length is SCS_ERR_INVAL: SCS pads nothing and truncates nothing,
 * because a length is a wire class here, not a buffer size.
 * SPENDS A CREDIT. With no Send Credit the message enters Credit Wait if a
 * pool is bound, else SCS_ERR_NOCREDIT. */
int scs_fsm_send_msg(struct scs_fsm *f, vms_conid_t local_conid,
		     const uint8_t *body, uint32_t len);

/* ==========================================================================
 * WHY THE LAST SEND ON A CONNECTION WAS REFUSED (integration note E70)
 *
 * `err` is this FSM's own `enum scs_err` -- not the many-to-one SS$_ status
 * the glue answers with -- and `port_rc` is the injected send op's verbatim
 * refusal when the frame was refused BELOW this layer (0 otherwise, an honest
 * "not applicable" and never a success claim). The rest is the LIVE state of
 * the CDT at readback, so a reader can tell "the connection was not sendable"
 * apart from "it was sendable and something under it refused", and can see the
 * Send Credit and the peer that the answer belongs to. Every field is a read
 * of a real CDT (INV-6); nothing here is derived from the status.
 *
 * `port_was_refuser` exists so a CALLER OUTSIDE THIS LAYER can act on the
 * distinction without importing `enum scs_err`'s vocabulary -- the connection
 * manager's glue asks the PORT for its own reason when this is set, and asks
 * nothing when it is clear.
 * ========================================================================== */
struct scs_send_refusal {
	int32_t         err;              /* enum scs_err, 0 = none refused */
	int32_t         port_rc;          /* the send op's verbatim return  */
	uint32_t        refusals;         /* sends this CDT has refused     */
	vms_scs_sysid_t peer_sysid;       /* the system this CDT rides      */
	uint16_t        credit_send;      /* LIVE Send Credit               */
	uint8_t         cdt_state;        /* enum vms_scs_cdt_state, LIVE   */
	uint8_t         port_was_refuser; /* err == SCS_ERR_TXFAIL          */
};

/*
 * Fill `*out` for `local_conid`. Returns SCS_OK when the CDT exists (even if
 * it has refused nothing: `err` is then 0, which reads as "nothing refused"
 * and not as a refusal), and SCS_ERR_NOCONN when the Con.ID names no live CDT
 * -- which is itself the answer to "why was the send refused" in that case,
 * and the reason this refuses rather than zero-filling (INV-6).
 */
int scs_fsm_send_refusal(struct scs_fsm *f, vms_conid_t local_conid,
			 struct scs_send_refusal *out);

/* The local SYSAP has finished with `n` received buffers: they move from
 * `held` to `pending` and go out on the next message, or immediately in a
 * special credit message if Receive Credit is dangerously low. */
int scs_fsm_return_credit(struct scs_fsm *f, vms_conid_t local_conid, uint16_t n);

/* ==========================================================================
 * 9. Input -- the port's pe_upper_ops, bound to this FSM by the glue
 * ========================================================================== */

/* One received sequenced message: the WHOLE frame as the port delivered it
 * (vms_pe_fsm.c's vc_deliver hands the frame up, not a slice), plus the
 * destination Con.ID the port already read out of abs 64. */
void scs_fsm_rx_message(struct scs_fsm *f, vms_scs_sysid_t from,
			vms_conid_t dst_conid,
			const uint8_t *frame, uint32_t len);

/* One received datagram (no Con.ID pair was read by the port). */
void scs_fsm_rx_datagram(struct scs_fsm *f, vms_scs_sysid_t from,
			 const uint8_t *frame, uint32_t len);

/* The circuit to `peer` came up. */
void scs_fsm_vc_up(struct scs_fsm *f, vms_scs_sysid_t peer);

/*
 * THE VC-BREAK CONTRACT (design SS3.2.5, the FC-P2.2 half of the E10 ruling).
 * The circuit to `peer` is GONE. Walk that SB's CDT queue and, for EVERY CDT
 * on it: move it to CLOSED with reason SCS_CLOSE_PATHLOST, DISCARD the credit
 * ledger (a re-formed circuit starts new connections with fresh credits), fail
 * every send in Credit Wait with path-lost, and call the SYSAP's closed()
 * (the `disconnected()` the design names -- see vms_scs.h).
 *
 * NOTHING IS PUT ON THE WIRE and nothing is retried: SCS does not re-open a
 * connection on its own. CNXMAN's recnx_fsm is the SYSAP that reconnects.
 * `reason` is the port's `enum pe_vc_down_reason`, carried through for the
 * console line; it does not change what SCS does.
 */
void scs_fsm_vc_down(struct scs_fsm *f, vms_scs_sysid_t peer, uint32_t reason);

/* A timer fired. `key` is the CDT's CDL index (its `timer_key`). */
void scs_fsm_timer(struct scs_fsm *f, enum scs_timer which, uint32_t key);

/* ==========================================================================
 * 10. Lookup and readback -- the same structs the diagnostics ioctl projects
 * ========================================================================== */

/* The CDT a Con.ID names, via the CDL (ch. 2's low-16-bits index) with the
 * FULL value verified. NULL for a stale or foreign Con.ID -- never a
 * plausible-looking neighbour. */
struct scs_cdt *scs_fsm_cdt_by_conid(struct scs_fsm *f, vms_conid_t conid);
struct scs_cdt *scs_fsm_cdt_at(struct scs_fsm *f, uint32_t index);
struct scs_sb  *scs_fsm_sb_by_sysid(struct scs_fsm *f, vms_scs_sysid_t sysid);

/* The `index`-th SB, or NULL past the table's end / on a free slot. The twin
 * of scs_fsm_cdt_at() above, and the enumerator CNXMAN's peer sweep needs
 * (FC-P3.9): a SYSAP cannot discover WHICH systems the port has circuits to
 * from scs_fsm_sb_by_sysid() alone, because asking that question requires
 * already knowing the answer. NULL for a free slot rather than a zeroed SB:
 * "no system here" is not "a system whose SCSSYSTEMID is 0". */
struct scs_sb  *scs_fsm_sb_at(struct scs_fsm *f, uint32_t index);

/* Pure projections into the frozen cross-substrate views (INV-6: what was
 * never learned stays zero with its flag clear). */
void scs_fsm_cdt_project(const struct scs_fsm *f, const struct scs_cdt *cdt,
			 struct vms_scs_cdt_view *out);
void scs_fsm_view_project(const struct scs_fsm *f, struct vms_scs_view *out);

#endif /* OVMX_VMS_SCS_FSM_H */
