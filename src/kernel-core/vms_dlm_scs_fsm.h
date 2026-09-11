/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_dlm_scs_fsm.h - the DLM's REQUESTER arm (plan item FC-P4.6; design
 * docs/design-faithful-cluster-executive.md §3.6 D-DLM-4/D-DLM-5; Davis
 * pp. 6-31/6-32 the three-outcome lookup, p. 6-50 the wire-carried hash,
 * p. 6-52 the process copy).
 *
 * WHAT THIS IS. One node's side of "a $ENQ named a resource somebody else
 * masters". The lock ENGINE (src/kernel-core/vms_lock.c) already creates the
 * real object for it -- the PROXY LKB, the process copy -- resolves the
 * directory, posts the request and sleeps the caller on the LKB's condition
 * variable. This file is what happens in between: it turns that post into a
 * cat-0x02 frame, follows the answer through the directory to the master,
 * completes the lock, carries the blocking AST back, and gives up HONESTLY when
 * no answer is coming.
 *
 * WHAT IT IS NOT. It owns no lock state, no connection, no timer and no clock.
 * `struct dlm_req` below carries a lock ID, a destination, a state and a retry
 * count -- and NOT ONE value that goes on the wire as content. Every such value
 * is fetched, at the moment the frame is built, through `ops->refill_post`,
 * which is vms_lock.c's dlm_proxy_fill_post() reading the real LKB and RSB.
 *
 * ==========================================================================
 * INV-6, STRUCTURALLY: WHY THIS FSM CANNOT PLUMB A FIELD FRAME-TO-FRAME
 * ==========================================================================
 * The failure this file is written against is documented and it killed a real
 * cluster twice: a completion frame carrying a PLACEHOLDER lock id bugchecked
 * VAX1 with INVLOCKID (commit fc8540ae), and a directory lookup carrying a
 * SELF-COMPUTED hash made a real VAX install OVMX as master of resources it did
 * not master -- the 35/s grant storm (memory cluster-promotion-gap).
 *
 * Both are the same bug: a value asserted on the wire that no executive read
 * produced. So:
 *
 *   1. THE FSM HAS NO FIELD TO PLUMB. Look at `struct dlm_req`: no resource
 *      name, no mode, no master handle, no value block, no hash. It physically
 *      cannot carry one across two frames, because it does not have anywhere to
 *      put it.
 *   2. EVERY FRAME IS BUILT FROM A FRESH `refill_post`. The first
 *      transmission, every retransmit, and every retry at a new target. Not
 *      one of them is built from the reply that arrived a microsecond
 *      earlier. (The post-grant COMPLETION that used to be the sharpest
 *      example of this rule is gone -- see §"WHAT IS GROUNDED" -- because the
 *      real protocol has no such frame. The rule is unchanged; it now has one
 *      fewer frame to apply to, which is the safest direction for it to
 *      move.)
 *   3. AN ANSWER GOES INTO THE LOCK DATABASE BEFORE IT IS USED. When the
 *      directory names a master, the FSM calls `ops->record_master` and then
 *      REFILLS; the retry's destination and its `master_csid` field come back
 *      out of the executive. The CSID is never carried from the reply frame to
 *      the request frame inside this object.
 *   4. THE HASH IS NEVER DERIVED, HERE OR ANYWHERE. `dir_resolve` takes a
 *      16-bit VALUE, not a name (vms_dlm_proxy.h states why). A request whose
 *      proxy post carries `dir_hash_known == 0` and which must be addressed to
 *      a DIRECTORY is REFUSED with nothing sent -- `hash_unknown_refused`,
 *      counted. That refusal is the grant storm's cure.
 *
 * ==========================================================================
 * WHAT IS GROUNDED, AND WHERE THIS FILE HONESTLY STOPS
 * ==========================================================================
 * vms_cluster_codec_dlm.h draws the line and this file respects it exactly.
 *
 *   GROUNDED, and therefore BUILT and SENT here:
 *     op 0x01 ENQ request, op 0x07 CONVERT request  (spec §4(f).1)
 *     the cat-0x82 reply's GRANT vs DENY shape       (spec §4(f).1)
 *
 *   AND THAT IS THE WHOLE OUTBOUND SET, INCLUDING AFTER A GRANT.
 *
 *     An earlier revision sent a PROVISIONAL "op 0x04 completion + op 0x03
 *     commit" pair once a grant landed. The vms-c03 capture of a real 2-node
 *     OpenVMS VAX 7.3 cluster showed that pair does not exist: 0x03 is $DEQ,
 *     0x04 is BLKAST, 0x06 carries the value block, and a real requester
 *     answers a grant with NO frame whatsoever (vms_cluster_codec_dlm.h's
 *     supersession note). So the pair is gone from this file, which makes
 *     OVMX emit strictly FEWER frame shapes than before -- a divergence
 *     REDUCTION, not a lost capability.
 *
 *     THE GRANT IS THE TERMINAL SETTLE. `grant_recv` puts the master's handle
 *     in the executive's lock record and the block goes straight to
 *     ST_GRANTED with `settled` set, which the beat skips. Nothing is owed,
 *     nothing is retransmitted, and no request slot is held open for an
 *     acknowledgement -- so a granted lock is immediately usable (a CONVERT
 *     posts and transmits) and immediately releasable (a $DEQ post frees the
 *     block). Nothing is stranded at the far end either: the DLM's inbound
 *     arm (vms_dlm_scs.c) serves only ENQ/CONVERT/REBUILD and DECLINES
 *     everything else, so no OVMX master ever consumed the pair.
 *
 *   GROUNDED IN THE CODEC, BUT NOT TRANSMITTED BY THIS ARM:
 *
 *     THE RELEASE (a cross-node $DEQ), op 0x03. vms-c03 grounded the opcode
 *     and vms_cluster_codec_dlm.h now carries a real parser and a real
 *     builder for it. What has NOT happened is the separate, lab-gated step
 *     of letting this arm put one on a live cluster's wire: a new outbound
 *     frame shape is a peer-crash vector until a real peer has been seen to
 *     take it (memory ovmx-never-crashes-a-peer), and that proof belongs to
 *     its own item. So a POST_DEQ is still REFUSED (DLM_REQ_E_NOWIREOP) and
 *     still COUNTED in `releases_no_wire_op` -- a measured, reportable gap,
 *     not a silent one, and no longer a gap in the FIELD MAP. THIS REMAINS
 *     THE OPEN HALF OF INTEGRATION NOTE E6: rundown COLLECTS the release and
 *     posts it from a blockable context (lock_sweep_run); what it does not
 *     yet do is transmit it.
 *
 *     THE VALUE BLOCK, op 0x06. vms-c03 grounded the 16 bytes at body[36:52]
 *     and the codec has an ACCESSOR for them; it deliberately has no builder,
 *     because the four bytes ahead of the block are not pinned by any capture
 *     and composing a whole op-0x06 frame would mean minting them. So the
 *     write crossing is still NOT transmitted (`lvb_write_no_wire_field`), and
 *     an inbound grant is still handed to the engine with `valblk_present = 0`
 *     -- the op-0x01 grant genuinely carries no value block -- which makes the
 *     engine leave the proxy's own block alone rather than overwrite it with
 *     zeros (vms_dlm_proxy.h `struct vms_dlm_proxy_grant`). Sixteen zeros
 *     presented as an LVB is a placeholder that reads exactly like data, which
 *     is the worst kind.
 *
 *     THE BLOCKING AST, op 0x04. Grounded by vms-c03 and parseable by the
 *     codec, which identifies its lock by `master_lkid` and by nothing else
 *     (the reference frame's readable body[48] resource name is STALE BUFFER
 *     belonging to another lock, and the codec refuses to read it). This
 *     file's BLKAST entry point still takes a lock id rather than a frame:
 *     FC-P4.8's classifier is what must raise it from the parsed frame, and
 *     that wiring is its own item.
 *
 *     THE DIRECTORY's outcome-2/3 reply shapes. This file has an ENTRY POINT
 *     for each -- the model needs them and the simulator drives them -- but no
 *     PARSER, because there is still nothing grounded to parse against.
 *
 * ==========================================================================
 * CONTEXT (design §3.2.6, E42/E45)
 * ==========================================================================
 * Every entry point here is non-blocking, allocation-free and clock-free (it
 * reads time only through `ops->now_ms`). The GLUE (FC-P4.8) serializes them
 * under one mutex; this object holds no lock of its own, so that mutex is a
 * leaf. `ops->send` hands a body to the connection manager for transmission on
 * the fork thread and must not wait for it; `ops->refill_post` and the other
 * engine doors take a resource lock briefly and never sleep.
 *
 * INCLUDES: kernel-core headers only (CI gate
 * tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_DLM_SCS_FSM_H
#define OVMX_VMS_DLM_SCS_FSM_H

#include "vms_cluster.h"            /* vms_csid_t                            */
#include "vms_cluster_codec_dlm.h"  /* the ONLY path to a cat-0x02 byte       */
#include "vms_dlm_proxy.h"          /* struct vms_dlm_proxy_post / _grant     */

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 1. Sizing and budgets -- OVMX DESIGN VALUES, labelled as such
 *
 * No published VMS limit is in this project's sources. Exceeding one is an
 * HONEST, COUNTED REFUSAL, never a silently evicted request: a request this
 * node forgot about is a lock the master still thinks we hold.
 * ========================================================================== */

/* Cross-node requests outstanding at once. */
#define DLM_REQ_MAX 32u

/*
 * The cat-0x02 SYSAP body length -- the WHOLE body of the 190-byte
 * VMS_FCLS_SCS_MSG frame every DLM message rides (spec §4(f); the codec states
 * the same number as VMS_DLM_REBUILD_ECHO_LEN, "body[0:132), abs 72..204", and
 * this is that number under a name that is not about rebuild records). Aliased
 * rather than re-spelled so the two can never drift.
 */
#define DLM_REQ_BODY_LEN VMS_DLM_REBUILD_ECHO_LEN

/*
 * The retransmit ladder. A request that has been transmitted this many times
 * without an answer is FAILED with a real terminal status through `ops->fail`,
 * so a $ENQW returns to its caller instead of hanging forever. The strawman
 * had no such bound and re-sent at 35/s.
 */
#define DLM_REQ_MAX_TRIES 5u

/* Milliseconds between transmissions of an unanswered request. An OVMX design
 * value: the wire has no grounded DLM request timeout. */
#define DLM_REQ_RETRY_MS 2000u

/*
 * How many times a DECLINE may re-resolve and retry. Bounded separately from
 * the retransmit ladder because a decline is an ANSWER (the target is not the
 * directory/master for this name) and re-resolving is the correct response --
 * but only while the vector keeps giving a DIFFERENT answer. Retrying the same
 * target after the same decline is the grant storm.
 */
#define DLM_REQ_MAX_REDIRECTS 4u

/* ==========================================================================
 * 2. Results -- this file's OWN vocabulary
 *
 * Deliberately NOT SS$_ codes: this TU is pure and compiles on the host with no
 * vms_internal.h in scope. The GLUE maps these at its own boundary, exactly as
 * vms_dlm_ldwv.h's enum is mapped by the lock engine.
 * ========================================================================== */
enum dlm_req_status {
	DLM_REQ_OK = 0,
	DLM_REQ_E_INVAL,      /* a null argument or a zero lock id             */
	DLM_REQ_E_NOSLOT,     /* the request table is full -- counted refusal  */
	DLM_REQ_E_NOLOCK,     /* the engine holds no proxy by that handle      */
	DLM_REQ_E_NOHASH,     /* no WIRE-LEARNED hash: send NOTHING            */
	DLM_REQ_E_NODIR,      /* the weight vector resolved no directory node  */
	DLM_REQ_E_NOWIREOP,   /* no GROUNDED cat-0x02 opcode for this operation*/
	DLM_REQ_E_CODEC,      /* the codec refused to build the frame          */
	DLM_REQ_E_SEND,       /* the connection manager could not take it      */
	DLM_REQ_E_STATE       /* an empty [state][event] cell -- counted       */
};

/*
 * Why a request ended without a grant. The GLUE maps each to the SS$_ status a
 * $ENQW's caller sees; this file names the FACT, not the code.
 */
enum dlm_req_fail_reason {
	DLM_REQ_FAIL_TIMEOUT = 0,      /* the ladder is spent                  */
	DLM_REQ_FAIL_NOTQUEUED,        /* the MASTER really declined it        */
	DLM_REQ_FAIL_PATHLOST,         /* the target left the cluster          */
	DLM_REQ_FAIL_UNROUTABLE        /* no hash / no directory / no wire op  */
};

/* ==========================================================================
 * 3. WHAT A GRANT TOLD US -- the parsed reply, in this file's vocabulary
 *
 * Filled by this file from the codec's own accessors plus the frame's SCA
 * source address, and handed straight to the engine. See
 * vms_dlm_proxy.h's `struct vms_dlm_proxy_grant` for the field-by-field
 * sourcing note, above all why `valblk_present` exists.
 * ========================================================================== */

/* ==========================================================================
 * 4. The injected ops -- every door out of this file
 * ========================================================================== */
struct dlm_req_ops {
	/*
	 * Send one cat-0x02 BODY (byte 0 == frame-absolute 72) to `dst_csid` on
	 * the VMS$VAXcluster connection. Production: vms_dlm_scs_post_request
	 * through the glue, which queues it to the fork thread. Same shape as
	 * cnxman_ops.send, and for the same reason: this layer cannot send, it
	 * can only ask. Returns 0 when the body was taken.
	 */
	int (*send)(void *ctx, vms_csid_t dst_csid, const uint8_t *body,
		    uint32_t len);

	/*
	 * *** THE ANTI-LARP DOOR. *** Re-read the proxy LKB named by `req_lkid`
	 * into a FRESH post -- production: vms_lock_dlm_proxy_refill_post,
	 * i.e. vms_lock.c's one INV-6 chokepoint reading the real LKB and RSB.
	 * `op` and `dst_csid` are THIS transmission's routing decision (the only
	 * two values this file owns); everything else comes out of the lock
	 * database. Returns 0, or non-zero when the lock is gone -- and then the
	 * transmission is ABANDONED, because a frame about a lock that no longer
	 * exists is a frame with no object behind it.
	 */
	int (*refill_post)(void *ctx, uint32_t req_lkid, uint32_t op,
			   vms_csid_t dst_csid, struct vms_dlm_proxy_post *out);

	/*
	 * Which member is the directory node for the root name whose
	 * WIRE-LEARNED hash is `hash16`? The connection manager's Lock Directory
	 * Weight Vector, `ldwv[hash16 mod n]` (vms_dlm_ldwv.h). 0 in *out_csid
	 * means THIS NODE. Non-zero return means "not resolved", and then
	 * nothing is sent. There is deliberately no variant taking a NAME.
	 */
	int (*dir_resolve)(void *ctx, uint16_t hash16, vms_csid_t *out_csid);

	/* The vector's generation. It changes at Phase 1 of every transition
	 * (Davis p. 6-33), which is how a routing decision made under an old
	 * vector is detected rather than remembered. */
	uint32_t (*dir_generation)(void *ctx);

	/* --- the engine ACTIONS (vms_dlm_proxy.h) --- */

	/* Outcome 2: record the master the directory named, THEN refill. */
	int (*record_master)(void *ctx, const char *resnam, uint32_t req_lkid,
			     vms_csid_t master_csid);
	/* Outcome 3: "you master it" -- promote the proxy and run local
	 * granting, so the $ENQW completes from a genuine local grant. */
	int (*assume_mastery)(void *ctx, const char *resnam, uint32_t req_lkid);
	/* A grant arrived: record it on the proxy and wake the waiter. */
	int (*grant_recv)(void *ctx, const struct vms_dlm_proxy_grant *g);
	/* A blocking AST arrived for a lock we hold: fire the holder's REAL
	 * user-mode AST. Non-zero means nothing was delivered (no proxy, no
	 * routine, no owner) -- honest, never faked. */
	int (*blkast_deliver)(void *ctx, uint32_t req_lkid);
	/* Learn a root name's directory hash from a frame that carried it
	 * (Davis p. 6-50) -- production: vms_lock_dlm_learn_dir_hash. */
	int (*learn_dir_hash)(void *ctx, const char *resnam, uint16_t hash16);
	/* No answer is coming: end the proxy's wait with a real status. */
	void (*fail)(void *ctx, uint32_t req_lkid, enum dlm_req_fail_reason why);

	uint32_t (*now_ms)(void *ctx);
	void     (*log)(void *ctx, const char *msg);
	void    *ctx;
};

/* ==========================================================================
 * 5. The state and event vocabulary -- the table's two axes
 *
 * ONE REQUEST BLOCK PER OUTSTANDING CROSS-NODE OPERATION, and its state is
 * WHERE THE REQUEST IS, which is exactly the published three-outcome walk:
 * at the directory (LOOKUP), at the master (ENQ), settled (GRANTED).
 * ========================================================================== */
enum dlm_req_state {
	DLM_REQ_ST_IDLE = 0,   /* a free slot                                  */
	DLM_REQ_ST_LOOKUP,     /* a lookup is outstanding at the DIRECTORY node*/
	DLM_REQ_ST_ENQ,        /* an ENQ/CONVERT is outstanding at the MASTER  */
	DLM_REQ_ST_GRANTED,    /* the master granted; the proxy is a real lock */
	DLM_REQ_ST__COUNT
};

enum dlm_req_event {
	DLM_REQ_EV_ENQ = 0,    /* the engine posted a NEW lock request         */
	DLM_REQ_EV_CONVERT,    /* the engine posted a mode CONVERT             */
	DLM_REQ_EV_DEQ,        /* the engine posted a RELEASE                  */
	DLM_REQ_EV_GRANT,      /* a cat-0x82 reply, GRANTED shape              */
	DLM_REQ_EV_DENY,       /* a cat-0x82 reply, DENIED shape               */
	DLM_REQ_EV_REDIRECT,   /* outcome 2: "the master is X"                 */
	DLM_REQ_EV_ASSUME,     /* outcome 3: "no master -- you master it"      */
	DLM_REQ_EV_BLKAST,     /* the master is blocked behind our lock        */
	DLM_REQ_EV_TIMEOUT,    /* the retransmit deadline expired              */
	DLM_REQ_EV_PEER_GONE,  /* the target left the cluster                  */
	DLM_REQ_EV__COUNT
};

/* ==========================================================================
 * 6. One outstanding cross-node request
 *
 * READ THE FIELD LIST AS THE INV-6 PROOF IT IS. There is no resource name, no
 * lock mode, no master lock id, no value block and no hash here. Those are wire
 * CONTENT and they live in the lock database; this block holds only the key,
 * the routing decision, and the bookkeeping a retry needs.
 * ========================================================================== */
struct dlm_req {
	uint8_t    state;          /* enum dlm_req_state                       */
	uint8_t    to_directory;   /* is dst the DIRECTORY or the MASTER?      */
	uint8_t    tries;          /* transmissions of the current frame       */
	uint8_t    redirects;      /* declines/redirects followed so far       */

	/*
	 * NOTHING IS OUTSTANDING ON THE WIRE FOR THIS BLOCK.
	 *
	 * Set the moment a grant arrives: a real VMS requester answers a grant
	 * with no frame at all (the vms-c03 supersession, see §"WHAT IS
	 * GROUNDED"), so the grant IS the terminal settle. The block lives on
	 * as this arm's wire record of a cross-node lock we hold -- so a
	 * BLKAST, a CONVERT or a duplicate grant has something to land on --
	 * but the beat (§12) skips it, which is what guarantees there is no
	 * ladder running, no retransmit pending, and no slot held open waiting
	 * for an acknowledgement.
	 */
	uint8_t    settled;
	uint8_t    pad[3];

	/*
	 * The key. THIS node's own lock id for the proxy -- a value the
	 * executive minted, and the (req_csid, req_lkid) half that makes every
	 * retransmit idempotent at the master (D-DLM-5) and makes a duplicate
	 * reply find the ONE request it names.
	 */
	uint32_t   req_lkid;

	/* THIS transmission's routing decision -- the two values
	 * vms_dlm_proxy.h explicitly makes the caller's (see refill_post). */
	vms_csid_t dst_csid;
	uint32_t   post_op;        /* VMS_DLM_POST_ENQ / _CONVERT / _DEQ       */

	/* The directory vector's generation this routing decision was made
	 * under. A decline re-resolves; a resolution taken under a superseded
	 * vector is not reused. */
	uint32_t   dir_gen;

	uint32_t   sent_ms;        /* when the current frame went out          */
	uint32_t   frames_tx;      /* every transmission for this request      */
};

/* ==========================================================================
 * 7. The requester arm
 *
 * No globals (design §3.9 rule 3): one instance per node, so the rung-2
 * simulator runs the real object.
 * ========================================================================== */
struct dlm_req_fsm {
	const struct dlm_req_ops *ops;

	struct dlm_req req[DLM_REQ_MAX];

	/*
	 * The SPLICE scratch (the vms_mscp_cl_io_fsm.h §9 pattern, same
	 * reason). The codec addresses a cat-0x02 message at FRAME-ABSOLUTE
	 * offsets; a SYSAP is handed, and hands back, only its own body
	 * (byte 0 == abs 72). A request is built into this frame and sent from
	 * `txframe + VMS_OFF_SYSAP_BODY`. It is why this file contains no wire
	 * offset of its own beyond the codec's published body origin.
	 */
	uint8_t txframe[VMS_OFF_SYSAP_BODY + DLM_REQ_BODY_LEN];

	/* Real events, counted where they happen. Counted, never inferred, and
	 * never a reason to invent an answer. */
	uint32_t enqs_posted;
	uint32_t converts_posted;
	uint32_t lookups_sent;        /* addressed to a DIRECTORY node         */
	uint32_t requests_sent;       /* addressed to a MASTER                 */
	uint32_t retransmits;
	uint32_t grants_rx;
	uint32_t grants_duplicate;    /* a grant for an already-granted request*/
	uint32_t denies_rx;
	uint32_t redirects_followed;  /* outcome 2 -> retry at the named master*/
	uint32_t masteries_assumed;   /* outcome 3                             */
	uint32_t declines_reresolved; /* a decline -> re-resolve -> retry      */
	uint32_t grants_settled;      /* grants that reached the terminal      */
				       /* settled state (§"WHAT IS GROUNDED"):  */
				       /* the grant IS the settle, no frame     */
				       /* follows it                            */
	uint32_t blkasts_rx;
	uint32_t blkasts_delivered;   /* a REAL user-mode AST was queued       */
	uint32_t blkasts_undeliverable;
	uint32_t hashes_learned;      /* body[10:12] -> the resource block     */

	/* The refusals -- each one a place this file declines to fabricate. */
	uint32_t hash_unknown_refused;   /* a lookup with no wire-learned hash */
	uint32_t dir_unresolved;         /* the vector gave no directory node  */
	uint32_t releases_no_wire_op;    /* $DEQ: no grounded opcode (E6)      */
	uint32_t lvb_write_no_wire_field;/* the LVB write crossing, unsent     */
	uint32_t lock_gone;              /* refill found no proxy: abandoned   */
	uint32_t no_slot;
	uint32_t codec_failures;
	uint32_t send_failures;
	uint32_t replies_unmatched;      /* a reply naming no request of ours  */
	uint32_t replies_unparsed;
	uint32_t ignored_events;         /* an empty [state][event] cell       */
	uint32_t timeouts_failed;        /* the ladder was spent               */
	uint32_t peers_gone;
};

/* ==========================================================================
 * 8. Lifecycle
 * ========================================================================== */

/* Reset to an empty requester arm bound to `ops`. Builds and sends nothing. */
void dlm_req_fsm_init(struct dlm_req_fsm *f, const struct dlm_req_ops *ops);

/* ==========================================================================
 * 9. The OUTBOUND event: the engine posted a request
 *
 * This is what the glue's `vms_dlm_requester_ops.post` calls. `p` is the post
 * the engine's dlm_proxy_fill_post() JUST filled from the real LKB and RSB, so
 * the first transmission is built straight from it -- no refill, because it IS
 * a fresh executive read.
 *
 * Returns DLM_REQ_OK when a frame really went out (or, for a retransmit of a
 * request already outstanding, when the duplicate was recognised and NOT
 * re-minted). Any other return means NOTHING WAS SENT.
 * ========================================================================== */
enum dlm_req_status dlm_req_fsm_post(struct dlm_req_fsm *f,
				     const struct vms_dlm_proxy_post *p);

/* ==========================================================================
 * 10. The INBOUND events
 *
 * `frame` is the whole received frame, exactly as cnxman_barrier_rx_frame takes
 * it: the codec's cat-0x02 accessors address frame-absolute offsets, and
 * reconstructing an envelope around a body in order to classify it would be
 * putting bytes on a frame that nobody sent.
 * ========================================================================== */

/*
 * A cat-0x82 DLM reply arrived from `from_csid`. Parses it through the codec
 * (grant-vs-deny SHAPE, spec §4(f).1), matches it to a request of OURS, and
 * drives the table. A reply naming no request of ours is COUNTED and dropped --
 * never applied to a request it does not belong to.
 *
 * `correlated_lkid` is the request handle the CONNECTION MANAGER matched this
 * reply to through its OWN transaction envelope (spec §4(j): the send/ack
 * counters and the transaction token), or 0 when it has none. It WINS over the
 * body[20] lock-id, because the envelope's correlation is the one a real VAX
 * requires and the one this tree has grounded for a reply; §4(f).1 reads a
 * GRANT's body[20] as "the requester's real assigned lock-id", which is true of
 * the value THIS node put on its own request and is not something to rely on
 * when a foreign master rewrites the field.
 */
enum dlm_req_status dlm_req_fsm_reply(struct dlm_req_fsm *f,
				      vms_csid_t from_csid,
				      uint32_t correlated_lkid,
				      const uint8_t *frame, uint32_t len);

/*
 * OUTCOME 2 (Davis p. 6-31): the directory node answered "the master is
 * `master_csid`". NO GROUNDED cat-0x02 SHAPE CARRIES THIS TODAY -- §4(f).1
 * grounds the grant and deny shapes and nothing else -- so this is an explicit
 * entry point rather than something dlm_req_fsm_reply() infers. FC-P4.8's
 * classifier may raise it only from a genuinely sourced CSID (a frame's own SCA
 * source address is the one grounded candidate), and FC-P5.2's capture is what
 * grounds a reply shape if there is one.
 *
 * The FSM records the master IN THE LOCK DATABASE and then REFILLS, so the
 * retry's destination is an executive read.
 */
enum dlm_req_status dlm_req_fsm_redirect(struct dlm_req_fsm *f,
					 uint32_t req_lkid,
					 vms_csid_t master_csid);

/*
 * OUTCOME 3: "there is no master -- YOU master it". Same grounding note as
 * above. The FSM asks the engine to promote the proxy onto res->waiting and run
 * the local granting algorithm, so the requester's $ENQW completes from a real
 * local grant. It sends nothing: there is nobody to send to.
 */
enum dlm_req_status dlm_req_fsm_assume_mastery(struct dlm_req_fsm *f,
					       uint32_t req_lkid);

/*
 * The target DECLINED: it is not the directory (or not the master) for this
 * name. The correct answer is to RE-RESOLVE through the CURRENT vector and
 * retry the new target -- and to STOP when the vector keeps naming the same
 * one, because retrying the same target after the same decline is precisely the
 * 35/s grant storm. Bounded by DLM_REQ_MAX_REDIRECTS.
 */
enum dlm_req_status dlm_req_fsm_decline(struct dlm_req_fsm *f,
					uint32_t req_lkid);

/*
 * A BLOCKING AST arrived for the lock `req_lkid` -- the master has a
 * conflicting request queued behind ours. NO GROUNDED cat-0x02 BLKAST SHAPE
 * EXISTS (the codec says so and defines no parser), so this is an explicit
 * entry point; the handle it names is OUR OWN, so the object is found by a
 * value this executive minted.
 */
enum dlm_req_status dlm_req_fsm_blkast(struct dlm_req_fsm *f,
				       uint32_t req_lkid);

/* A member left the cluster. Every request outstanding at it is FAILED with a
 * real path-lost status, so no $ENQW waits for an answer that cannot come. A
 * GRANTED request is left alone -- the engine's own departure path owns what
 * happens to a lock whose master is gone. Returns how many were failed. */
uint32_t dlm_req_fsm_peer_gone(struct dlm_req_fsm *f, vms_csid_t csid);

/*
 * THE HASH LEARNER (integration note E49). Called for EVERY inbound cat-0x02
 * frame this node sees, in any role: a lookup received, a request received as
 * master, a rebuild registration, a deny that echoed the name. It reads the
 * SENDER's own 16-bit value at body[10:12] together with the root NAME in the
 * same frame and records the pair through `ops->learn_dir_hash` -- Davis p.
 * 6-50's "the receiving system uses the received value".
 *
 * This is the ONLY way `rsb->hash16` is ever set, and therefore the only reason
 * OVMX can address a directory lookup at all. Returns the number of hashes
 * learned from this frame (0 or 1).
 */
/*
 * THE BODY ENTRIES (rd vms-1ee) -- what the live receive path calls, because
 * SCS delivers a SYSAP its own 132 bytes (design sec 3.2.4). Same behaviour as
 * the frame entries over the same codec cores; see vms_cluster_codec_dlm.h's
 * body-relative note for why both spellings exist.
 */
enum dlm_req_status dlm_req_fsm_reply_body(struct dlm_req_fsm *f,
					   vms_csid_t from_csid,
					   uint32_t correlated_lkid,
					   const uint8_t *body, uint32_t len);
uint32_t dlm_req_fsm_observe_body(struct dlm_req_fsm *f, const uint8_t *body,
				  uint32_t len);

uint32_t dlm_req_fsm_observe(struct dlm_req_fsm *f, const uint8_t *frame,
			     uint32_t len);

/* The requester arm's own beat: retransmit every request past its deadline
 * (from a FRESH refill -- see ops->refill_post), and fail the ones whose ladder
 * is spent. Returns how many frames it retransmitted. */
uint32_t dlm_req_fsm_tick(struct dlm_req_fsm *f);

/* ==========================================================================
 * 11. Readback (the same values a diagnostic projects -- INV-6)
 * ========================================================================== */
const struct dlm_req *dlm_req_fsm_at(const struct dlm_req_fsm *f,
				     uint32_t index);
const struct dlm_req *dlm_req_fsm_find(const struct dlm_req_fsm *f,
				       uint32_t req_lkid);
uint32_t dlm_req_fsm_outstanding(const struct dlm_req_fsm *f);
const char *dlm_req_state_name(enum dlm_req_state s);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_DLM_SCS_FSM_H */
