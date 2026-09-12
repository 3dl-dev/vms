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
 *     op 0x03 $DEQ, the cross-node RELEASE           (vms-c03, and see the
 *                                                     TWO GATES note below)
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
 *   THE RELEASE (a cross-node $DEQ), op 0x03, AND THE TWO GATES IT RIDES
 *   BEHIND (rd vms-d7a3). vms-c03 grounded the opcode from a real 2-node
 *   OpenVMS VAX 7.3 cluster and vms_cluster_codec_dlm.h carries a real parser
 *   and a real builder for it, so this arm now TRANSMITS one: a POST_DEQ
 *   builds an op-0x03 naming the lock by the proxy LKB's own handle, the
 *   master's handle as the grant recorded it, and the mode the LKB holds as it
 *   is released -- three executive reads out of one fresh `refill_post`, and
 *   no other field, because a real DEQ carries no resource name (the reference
 *   frame's body[46] holds uninitialised bytes that differ between two
 *   specimens in one capture, which is the proof it is not a field).
 *
 *   GROUNDED IS NOT CLEARED, so the emission is gated TWICE and neither gate
 *   is this object's to relax (memory ovmx-never-crashes-a-peer):
 *
 *     1. THE ALL-OVMX GATE, `ops->all_ovmx` -- every member of this cluster
 *        proven to run this implementation. A frame shape no real VAX has yet
 *        been WATCHED to accept may not be addressed at one. An ABSENT op
 *        reads CLOSED: "nobody told us" and "every member is ours" are
 *        different facts, and only one of them may put a new shape on a wire.
 *     2. RULE C, per destination, in the CONNECTION MANAGER under `ops->send`
 *        (`csb->peer_is_ours`). Two gates, two layers, neither a substitute
 *        for the other -- a gate that is only upstream is a gate one new call
 *        site bypasses.
 *
 *   A release that either gate (or a missing route, or a lock id the codec
 *   refuses) stops is COUNTED in `releases_no_wire_op` and NOTHING is sent;
 *   one that goes out is counted in `releases_sent`. That closes the open half
 *   of integration note E6 -- rundown already COLLECTED the release and posted
 *   it from a blockable context (lock_sweep_run); this is the transmission --
 *   for an all-OVMX cluster, and leaves it honestly counted everywhere else.
 *
 *   THE RECEIVE HALF IS A SEPARATE RUNG AND IS NOT CLAIMED HERE. OVMX's master
 *   arm (vms_dlm_scs.c) serves op 0x01/0x07/0x0d and DECLINES an inbound
 *   op-0x03 -- counted, never acted on -- so a release that reaches an OVMX
 *   master today changes exactly as much lock state as it did when it was
 *   never sent: none. The emission is what this item lands; consuming one is
 *   its own item, with its own proof.
 *
 *   THE VALUE BLOCK, BOTH CROSSINGS NOW WIRED (vms-727). vms-c03 grounded the
 *   16 bytes at body[36:52]; the own-lab campaign then pinned the framing on
 *   both sides:
 *     - THE WRITE, op 0x06. body[32:36] is a per-lock SERIAL (body[32]) plus
 *       the cat-0x02 request stamp (body[34]=0x01), so the op-0x06 builder
 *       exists and this arm EMITS a holder's value-block flush on a demote
 *       from write mode (the codec's vms_dlm_valblk_convert_build).
 *     - THE READ, op 0x01 grant. A grant that returns the master resource's
 *       block carries it at the SAME body[36:52], marked by the grounded
 *       grant-with-valblk record (body[28]=0x10, body[32:36]=01 00 fa 00, the
 *       cat-0x82 REPLY stamp shape). The codec recognises it and h_grant hands
 *       the block to the engine as `valblk_present = 1`; a plain or stale grant
 *       still leaves the proxy's own block ALONE (never sixteen zeros read as
 *       data). The master returns the block ONLY when the resource holds a
 *       non-zero one (vms_dlm_master_result.valblk_present).
 *
 *   GROUNDED IN THE CODEC, BUT NOT TRANSMITTED BY THIS ARM:
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

	/*
	 * IS EVERY MEMBER OF THIS CLUSTER PROVEN TO RUN THIS IMPLEMENTATION?
	 * Production: vms_ldwv_all_ovmx() over the connection manager's own
	 * vector -- the SAME one fact the engine's `dir_groundable` reads, so a
	 * VAX joining closes both and a VAX leaving reopens both with no code
	 * path to go stale.
	 *
	 * It gates the frame shapes this arm has grounded but never yet watched
	 * a real peer take (today: the op-0x03 release). Non-zero means open.
	 * A NULL op is CLOSED -- see §"WHAT IS GROUNDED" for why an absent
	 * answer may not be read as a permissive one.
	 */
	int (*all_ovmx)(void *ctx);

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
	uint32_t releases_sent;       /* op-0x03 $DEQ frames really emitted    */
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
	uint32_t blkasts_unparsed;    /* an op-0x04 body the codec refused --  */
				       /* wrong cat/op, or a zero lock id       */
	uint32_t hashes_learned;      /* body[10:12] -> the resource block     */

	/* The refusals -- each one a place this file declines to fabricate. */
	uint32_t hash_unknown_refused;   /* a lookup with no wire-learned hash */
	uint32_t dir_unresolved;         /* the vector gave no directory node  */
	uint32_t releases_no_wire_op;    /* a $DEQ this arm could NOT put on   */
					  /* the wire: the all-OVMX gate closed,*/
					  /* no route, no master handle, or the */
					  /* connection manager (RULE C)        */
					  /* refused it. Nothing was sent.      */
	uint32_t posts_no_wireop;        /* a post whose operation has NO wire */
					  /* opcode at all -- unreachable for   */
					  /* the three the engine posts, kept   */
					  /* as a refusal, never a fall-through */
	uint32_t lvb_write_no_wire_field;/* a write crossing the codec refused */
	uint32_t lvb_writes_sent;        /* op-0x06 CONVERT-with-VALBLK emitted */
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
 * conflicting request queued behind ours. The handle it names is OUR OWN, so
 * the object is found by a value this executive minted.
 *
 * This is the HANDLE-taking entry: whatever recognised the event supplies the
 * lock id. The op-0x04 wire shape became grounded with the vms-c03 capture and
 * the codec now parses it, so the live receive path uses the BODY-taking entry
 * below; this one remains for a caller that already holds the handle (and is
 * what that entry calls once the codec has produced it).
 */
enum dlm_req_status dlm_req_fsm_blkast(struct dlm_req_fsm *f,
				       uint32_t req_lkid);

/*
 * THE SAME EVENT AS SCS REALLY DELIVERS IT (rd vms-c72): the cat-0x02 op-0x04
 * body, 132 bytes, from the master. Parsed through the codec -- which is what
 * applies the category/opcode gate and the fc8540ae refusal of a zero lock id
 * in either field -- and then dispatched by the handle at body[20:24], which is
 * the one this node minted for its own proxy.
 *
 * `from_csid` is the sender as the connection manager identified it. It is
 * deliberately NOT used to find the lock: the LOCK is the key, and this arm
 * keeps no table addressed by the master's CSID.
 *
 * Returns DLM_REQ_OK only when a real blocking AST was delivered to a real
 * holder. Every other return means NOTHING was delivered, and one counter says
 * which refusal it was.
 */
enum dlm_req_status dlm_req_fsm_blkast_body(struct dlm_req_fsm *f,
					    vms_csid_t from_csid,
					    const uint8_t *body, uint32_t len);

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

/* ==========================================================================
 * 12. THE RELEASE QUEUE -- how a $DEQ survives the death of its own LKB
 *     (rd vms-49f8)
 *
 * THE BUG THIS OBJECT EXISTS FOR, as measured on the live 2-node rig (PR #1174:
 * `releases_sent=0`, `releases_no_wire_op=0`, `posts_lock_gone=2`). Every other
 * outbound DLM frame is built on the fork thread from a FRESH executive read --
 * the glue queues the lock id, and `ops->refill_post` re-reads the proxy LKB at
 * the moment of transmission (§"THE ANTI-LARP PRIMITIVE", vms_dlm_proxy.h).
 * That discipline is exactly right for an ENQ, a CONVERT and every retransmit,
 * and it is IMPOSSIBLE for a release: the $DEQ *is* the destruction of the
 * proxy LKB. vms_deq_core tears the lock block down as soon as the post is
 * queued, so by the time the fork thread ran there was nothing left to read and
 * the transmission was (correctly, given what it knew) abandoned. The op-0x03
 * emit below it had no reachable caller at all. Image rundown
 * (dlm_release_batch_post) died the same way.
 *
 * THE FIX, AND WHY IT IS NOT A CACHE. A release is not an outstanding request
 * whose fields can still move: it is a COMPLETED EVENT. At the instant the
 * engine fills the post -- under res->lock, from the live LKB, through the one
 * INV-6 chokepoint dlm_proxy_fill_post() -- this node has given up its stake in
 * a remote-mastered lock, and none of the four values that describe that act
 * can ever change again (no convert can follow a release; the master's handle
 * was established by the master's own grant). So the fork thread emits from a
 * SNAPSHOT of that moment, and the snapshot is the truth: refilling later could
 * only ever produce "no such lock", which is not a fresher answer, it is no
 * answer. This queue is the thread crossing, nothing more -- process context
 * stages, the fork thread claims, and the frame is built from what the engine
 * really read.
 *
 * FOUR FIELDS, AND THE OMISSIONS ARE STRUCTURAL. A record carries the proxy
 * LKB's own handle, the master's handle for it, the mode it held as it went,
 * and the master's CSID -- which is precisely what a cat-0x02 op-0x03 asserts
 * (vms_cluster_codec_dlm.h) plus where to send it. It deliberately does NOT
 * carry the resource name, the value block or the directory hash: those are
 * fields of a REQUEST, they have no grounded place on a release, and a snapshot
 * that held them would be a snapshot a later edit could put on a wire. What is
 * not in the record cannot be asserted.
 *
 * WHAT IS STILL REFUSED. Nothing here relaxes a gate. A record whose
 * `master_lkid` is 0 -- a lock the master never named -- reaches the codec's
 * fc8540ae refusal exactly as before and NOTHING is sent (the placeholder lock
 * id that bugchecked a real VAX with INVLOCKID). The all-OVMX gate and RULE C
 * are applied by dlm_req_fsm_post() on the fork thread, after the claim, so a
 * staged release toward a peer this executive cannot prove runs this
 * implementation is still counted and dropped, never emitted.
 *
 * PURE, AND LOCKED BY ITS OWNER. This object makes no call, takes no lock and
 * reads no clock; the GLUE (vms_dlm_scs.c) owns one, serialises every call on
 * its own executive lock, and is the only thing that knows about threads. The
 * `seq` stamp is what makes a claim safe: a slot is claimed only by the exact
 * (index, generation) pair that was staged, so a stale or duplicated work item
 * finds nothing and is counted rather than emitting a release twice.
 * ========================================================================== */

/*
 * Releases that may be in flight between process context and the fork thread at
 * once. An OVMX design value (no published VMS limit is in this project's
 * sources): four times the engine's own rundown batch, so a full process
 * rundown sweep stages without refusing, and small enough that the whole queue
 * is a few hundred bytes inside the arm's single allocation. A FULL queue is an
 * honest counted refusal -- never an evicted release, because an evicted
 * release is a lock the master still believes we hold.
 */
#define DLM_RELQ_SLOTS 16u

/* One release, as the executive really performed it. */
struct dlm_relq_rec {
	uint32_t req_lkid;     /* the proxy LKB's own handle (never 0)        */
	uint32_t master_lkid;  /* the master's handle, as its grant recorded  */
	uint32_t dst_csid;     /* the MASTER -- a release never goes to a
				* directory node                              */
	uint8_t  mode;         /* the mode the LKB held as it was released    */
	uint8_t  pad[3];
};

struct dlm_relq_slot {
	struct dlm_relq_rec rec;
	uint32_t seq;          /* the generation this staging minted; 0 free  */
	uint8_t  busy;
	uint8_t  pad[3];
};

struct dlm_relq {
	struct dlm_relq_slot slot[DLM_RELQ_SLOTS];
	uint32_t next_seq;

	/* Counted facts, every one a thing that really happened. */
	uint32_t staged;        /* releases snapshotted from a live LKB       */
	uint32_t claimed;       /* ... and really handed to the FSM           */
	uint32_t abandoned;     /* staged, but the fork queue would not take
				 * the work item: the slot is given back      */
	uint32_t full_refused;  /* no slot: NOTHING is staged, nothing sent   */
	uint32_t stale_refused; /* a claim whose (index, generation) names no
				 * staged release -- counted, never guessed   */
};

/* Reset to an empty queue. Stages nothing and frees nothing. */
void dlm_relq_init(struct dlm_relq *q);

/*
 * SNAPSHOT one release out of the post the engine just filled from the live
 * proxy LKB, and return the (slot, generation) pair that names it.
 *
 * `p` must be a VMS_DLM_POST_DEQ post carrying a real req_lkid; anything else is
 * DLM_REQ_E_INVAL and nothing is staged. DLM_REQ_E_NOSLOT means the queue is
 * full: the caller must report a failure to the releaser, because no frame will
 * be built for a release that was never staged.
 */
enum dlm_req_status dlm_relq_stage(struct dlm_relq *q,
				   const struct vms_dlm_proxy_post *p,
				   uint32_t *out_slot, uint32_t *out_seq);

/*
 * CLAIM the release named by (`slot`, `seq`), freeing the slot, and write it out
 * as the post the requester FSM takes -- op VMS_DLM_POST_DEQ, the four recorded
 * values, and ZEROS everywhere else, so no request-shaped field can ride a
 * release. DLM_REQ_E_NOLOCK when that pair names no staged release.
 */
enum dlm_req_status dlm_relq_claim(struct dlm_relq *q, uint32_t slot,
				   uint32_t seq, struct vms_dlm_proxy_post *out);

/* Give back a slot whose work item never made it to the fork thread. Counted
 * as abandoned: a release that was staged and then dropped is a gap, and a gap
 * is a number, not a silence. */
void dlm_relq_abandon(struct dlm_relq *q, uint32_t slot, uint32_t seq);

/* How many releases are staged and not yet claimed. */
uint32_t dlm_relq_pending(const struct dlm_relq *q);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_DLM_SCS_FSM_H */
