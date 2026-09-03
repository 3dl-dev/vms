/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_dlm_proxy.h - the PROXY-LKB requester seam (FC-P4.4).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.4 (RSB/LKB/directory),
 * SS3.6 D-DLM-4/D-DLM-5, hard call 7 ("proxy LKB replaces vms_dlm_origin");
 * plan row FC-P4.4. Book grounding: docs/design-cluster-book-grounding.md SS1.1
 * (the three-outcome directory lookup), SS2.5 (the three LKB kinds: local copy,
 * master copy, PROCESS COPY -- Davis p. 6-52).
 *
 * WHAT A PROXY LKB IS. When a $ENQ names a resource whose tree is mastered on
 * ANOTHER node, the executive still creates a real lock block for it: the
 * requester-side image, VMS's *process copy*. It carries the master's identity
 * (master_csid) and, once the master answers, the master's own handle for the
 * lock (master_lkid). $GETLKI, $DEQ, convert, blocking-AST delivery and the
 * lock value block all operate on that ONE object -- which is why the ad-hoc
 * `vms_dlm_origin` side list it replaces is gone.
 *
 * WHAT THIS HEADER IS. The engine cannot send: sending is the connection
 * manager's job (vms_dlm_scs.h RULE A -- a reply never leaves by itself). So
 * `vms_lock.c` reaches the wire through ONE injected op, registered at cluster
 * start and torn down at cluster stop. With no ops registered the engine is
 * purely local and a remote-mastered resource is refused honestly
 * (SS$_UNSUPPORTED) -- never granted locally, never faked (INV-6).
 *
 * INV-6 IN THE SHAPE OF THE STRUCT. `struct vms_dlm_proxy_post` is filled by
 * ONE function in vms_lock.c that reads a real LKB and its RSB at the moment of
 * the post; there is no path by which a caller supplies a value the executive
 * does not hold. A placeholder lock id is structurally impossible: req_lkid IS
 * the proxy LKB's lock id (a lock id the executive minted), and the post is
 * refused if it is zero -- the engine-side twin of the codec's
 * VMS_DLM_LKID_UNSET refusal (vms_cluster_codec_dlm.c), which a real VAX
 * bugchecked (INVLOCKID) over.
 *
 * INCLUDES: kernel-core headers only (CI gate
 * tools/ci/cluster_core_includes_gate.sh RULE 1/2).
 *
 * AND DELIBERATELY *NOT* vms_internal.h ANY MORE (FC-P4.6). This header used
 * to pull the per-substrate executive struct twin in for two things: the
 * fixed-width types and LCK_VALBLK_SIZE. That made it impossible for the
 * requester FSM to include BOTH this seam and the cat-0x02 codec
 * (vms_cluster_codec_dlm.h) in one pure translation unit -- vms_internal.h
 * carries the engine's ioctl op family, whose short names collided with the
 * codec's wire opcodes. The types now come from vms_wire_types.h (the single
 * sanctioned selector) and the value-block length is stated here and
 * _Static_assert-ed against LCK_VALBLK_SIZE in vms_lock.c, where both are
 * visible. The consequence is the one that matters: this seam, the codec and
 * the FSM over them all compile with a plain host compiler and no kernel
 * headers at all -- the R1 rung.
 */
#ifndef OVMX_VMS_DLM_PROXY_H
#define OVMX_VMS_DLM_PROXY_H

#include "vms_wire_types.h"   /* fixed-width types, substrate-selected once */

/*
 * THE LOCK VALUE BLOCK's length. The executive's own LCK_VALBLK_SIZE, restated
 * here so this header needs no substrate struct twin, and CHECKED against it by
 * a _Static_assert in vms_lock.c (the one TU that sees both). Two spellings of
 * one number are only safe when a build failure is what happens if they drift.
 */
#define VMS_DLM_VALBLK_LEN 16u

/*
 * THE POST'S OWN OPERATION VOCABULARY (FC-P4.6).
 *
 * The engine used to hand its ioctl dispatch selector (VMS_DLM_OP_ENQ, ...)
 * through this seam, which had two problems. It leaked an ioctl ABI number into
 * a cluster interface, and -- worse -- it could not tell a fresh ENQ from a
 * CONVERT: `convert_proxy_request` posted VMS_DLM_OP_ENQ too, so the wire arm
 * had no way to choose between cat-0x02 op-0x01 and op-0x07. These three names
 * are this seam's own, they map 1:1 onto the operations the DLM actually has,
 * and the FSM's translation to a wire opcode is a table lookup.
 */
#define VMS_DLM_POST_ENQ     1u   /* a NEW lock request       -> wire op 0x01 */
#define VMS_DLM_POST_CONVERT 2u   /* an existing lock's mode  -> wire op 0x07 */
#define VMS_DLM_POST_DEQ     3u   /* release                  -> the DEQ path */

/*
 * THE UNSET LOCK ID. The executive's own convention throughout vms_lock.c: 0 is
 * "not a real lock yet", so a lock id of 0 is never a handle and must never
 * reach the wire. vms_cluster_codec_dlm.h mirrors this same constant on the
 * codec side (guarded, so the two spellings can never disagree) and refuses to
 * BUILD a frame carrying it; the engine refuses to POST or to ACCEPT one. A
 * placeholder lock id in a completion bugchecked a real VAX with INVLOCKID and
 * took the cluster with it (commit fc8540ae) -- which is why the refusal exists
 * at both ends and not just one.
 */
#ifndef VMS_DLM_LKID_UNSET
#define VMS_DLM_LKID_UNSET 0u
#endif

/*
 * One outbound request derived from one proxy LKB.
 *
 * EVERY field is read out of the LKB / RSB named by the request, at post time,
 * by dlm_proxy_fill_post() in vms_lock.c. Nothing here is plumbed frame-to-frame
 * and nothing is defaulted from a template: an implementation of `post` that
 * needs a value not in this struct must go and read the executive for it, not
 * invent one.
 */
struct vms_dlm_proxy_post {
	uint32_t op;           /* VMS_DLM_POST_ENQ / _CONVERT / _DEQ            */
	uint32_t dst_csid;     /* where it goes: the MASTER when known, else the
	                        * DIRECTORY node for the root name (the book's
	                        * three-outcome lookup, p. 6-31)                */
	uint32_t req_csid;     /* this node, read from the executive's own CSID */
	uint32_t req_lkid;     /* the proxy LKB's lock id -- never 0            */
	uint32_t master_csid;  /* the master, when the cluster has told us; 0 = to
	                        * be resolved by the directory node             */
	uint32_t master_lkid;  /* the master's handle for this lock; 0 until the
	                        * master's own reply named it                   */
	uint32_t lkmode;       /* requested mode, read from the LKB             */
	uint32_t flags;        /* LCK_M_* the requester supplied, read from the LKB */
	char     resnam[32];   /* the RSB's name                                */
	uint8_t  valblk[VMS_DLM_VALBLK_LEN];  /* the LKB's value block (a write
	                        * crossing carries it; otherwise zeros)         */

	/*
	 * THE ROOT NAME'S DIRECTORY HASH, read off the RESOURCE BLOCK (FC-P4.6).
	 *
	 * `res->hash16` / `res->hash_known` -- the value some system in this
	 * cluster put on the wire for this exact name and that
	 * vms_lock_dlm_learn_dir_hash() recorded here. It rides in this struct
	 * for the same reason every other field does: the wire arm must be able
	 * to place it on a directory lookup (cat-0x02 body[10:12]) WITHOUT
	 * deriving it, and the only non-deriving source is an executive read at
	 * post time. `dir_hash_known` 0 means this executive holds none, and
	 * then a directory LOOKUP must not be sent at all -- SS$_UNSUPPORTED,
	 * the honest floor (design §3.6 rung A').
	 */
	uint16_t dir_hash;
	uint8_t  dir_hash_known;

	/*
	 * Is `dst_csid` the tree's MASTER (as the cluster told us) or the
	 * DIRECTORY node the weight vector named? Read from the LKB/RSB at post
	 * time -- `master_csid != 0 && dst_csid == master_csid` -- and recorded
	 * because the two are DIFFERENT operations on the wire: a request to
	 * the master needs no directory index, and a lookup to the directory
	 * cannot be sent without one. Deriving this in the wire arm from the
	 * two CSIDs would work today and silently stop working the day a
	 * directory node is also the master (Davis p. 6-31 outcome 1), which is
	 * the common case, so the engine states which it meant.
	 */
	uint8_t  to_directory;
};

/*
 * The requester ops. `post` hands the request to the connection manager's fork
 * context; it MUST NOT block and MUST NOT wait for the answer -- the engine owns
 * the wait, because the engine owns the LKB (vms_dlm_scs.h SS5). It returns
 * SS$_NORMAL when the request is queued for transmission, or an SS$_ status.
 */
struct vms_dlm_requester_ops {
	uint32_t (*post)(void *ctx, const struct vms_dlm_proxy_post *p);

	/*
	 * DIRECTORY RESOLUTION (FC-P4.3). "Which member is the directory node
	 * for the root resource whose 16-bit hash is `hash16`?" -- the
	 * connection manager's Lock Directory Weight Vector, indexed by
	 * `hash16 mod n` (Davis p. 6-31; vms_dlm_ldwv.h). Returns SS$_NORMAL
	 * and writes `*out_csid` only when the answer is REAL; 0 there means
	 * THIS NODE is the directory (p. 6-32: a system's own entries read 0 in
	 * its own copy). Any other return means "not resolved", and the engine
	 * must not proceed with a guess.
	 *
	 * THE HASH IS A PARAMETER, AND THAT IS THE POINT. It is not derived
	 * here and it is not derived by the implementation: it is the value the
	 * cluster itself put on the wire for that resource name (p. 6-50),
	 * learned into the RSB by vms_lock_dlm_learn_dir_hash() below. There is
	 * deliberately no variant of this op that takes a resource NAME -- an op
	 * that took a name would be an op somebody could implement by hashing
	 * it, which is exactly the thing that broke a real cluster (commit
	 * 90b3bbbd; design SS3.6).
	 *
	 * ABSENT (NULL) MEANS "NO CLUSTER", NOT "REFUSE". With no ops installed
	 * this node is alone: it is trivially the directory and the master for
	 * everything, and local locking is unaffected. It is only when a
	 * resolver IS installed -- i.e. this node is in a cluster -- that a
	 * resource with no wire-learned hash is refused.
	 *
	 * CONTEXT, and it is strict. The engine calls this HOLDING the resource
	 * lock (and, on the readback path, the resource-hash lock). Like `post`
	 * it MUST NOT block and MUST NOT re-enter the lock manager. It is a
	 * table read -- `entry[hash mod n]` over a vector the connection manager
	 * already built -- and nothing more; an implementation that needed to
	 * wait for the cluster to answer would be implementing rung B (probing),
	 * which this design does not take.
	 */
	uint32_t (*dir_resolve)(void *ctx, uint16_t hash16, uint32_t *out_csid);

	/*
	 * The directory vector's GENERATION. It changes whenever the vector
	 * changes -- including at Phase 1 of every state transition, when the
	 * book discards all directory information cluster-wide (p. 6-33).
	 *
	 * This is how "all rsb->dir_csid invalidated on vector change" is
	 * ENFORCED rather than remembered: the engine stores the generation
	 * beside each cached resolution and re-resolves the moment they differ,
	 * so a cached directory cannot outlive the vector it came from even if
	 * somebody forgets to walk the resource database. Absent (NULL) reads
	 * as generation 0, which is correct for "no cluster".
	 */
	uint32_t (*dir_generation)(void *ctx);

	void *ctx;
};

/*
 * LEARN A ROOT NAME'S DIRECTORY HASH FROM THE WIRE (FC-P4.3).
 *
 * Called by the DLM's wire arm for EVERY inbound cat-0x02 frame that carries
 * the field (body[10:12], vms_cluster_codec_dlm.h): lookups received, requests
 * received as master, and rebuild registrations. `hash16` is the value the
 * SENDING system put there; it is stored on the resource block with
 * `hash_known` set, and it is the only thing a directory lookup for that name
 * may ever be sent with.
 *
 * It creates the resource block if this node holds none: a cat-0x02 frame is
 * addressed to this node on the VMS$VAXcluster connection, and recording what
 * the cluster told us about a resource is what a resource database is for
 * (the directory node's own entry, p. 6-50, is the same idea; FC-P5.4 gives
 * that entry its master CSID and its directory-entry flag). A learned hash is
 * a preservation reason, so the block is not reclaimed the moment nothing is
 * locked on it -- otherwise the value would be thrown away exactly when it is
 * about to be needed.
 *
 * Returns SS$_NORMAL when the value was learned or already agreed, and an SS$_
 * status otherwise. A value that DISAGREES with one already learned for the
 * same name is refused with SS$_BADPARAM and counted: the first value stands
 * (routing must not churn), and a rising count falsifies either the field
 * offset or the "one hash per name, cluster-wide" property the whole scheme
 * rests on.
 */
uint32_t vms_lock_dlm_learn_dir_hash(const char *resnam, uint16_t hash16);

/* How many learned hashes disagreed with a value already held for that name
 * (see above). Instrumentation for the FC-P4.2 offset check; a diagnostic
 * reads it, nothing acts on it. */
uint32_t vms_lock_dlm_dir_hash_conflicts(void);

/*
 * WHERE `post` COMES FROM. The DLM's wire arm implements it (FC-P4.6): it
 * builds the cat-02 frame from the fields above through the codec and hands it
 * to the connection manager -- vms_dlm_scs.h's vms_dlm_scs_post_request, which
 * documents the same contract from the other side ("It does NOT wait: the
 * engine owns the wait, because the engine owns the LKB"). Nothing between here
 * and the wire may add a value of its own: if a frame field is not in the struct
 * above, its source is an executive read this layer has not yet been given, not
 * a default.
 */

/*
 * Install / remove the requester ops. `ops` NULL restores purely local
 * operation (a node with no cluster still locks; a remote-mastered resource is
 * then refused SS$_UNSUPPORTED rather than served from thin air).
 */
void vms_lock_dlm_set_requester_ops(const struct vms_dlm_requester_ops *ops);

/* ==========================================================================
 * WHAT THE WIRE ARM MAY ASK THE ENGINE FOR (FC-P4.6)
 *
 * Three reads/actions, and between them they are why the requester FSM never
 * needs to remember a wire value. All three are safe from the cluster fork
 * thread: each takes the resource lock briefly and none of them sleeps, waits
 * or allocates (design §3.2.6 E42/E45 -- no blocking work on the fork thread).
 * ========================================================================== */

/*
 * RE-READ the proxy LKB named by `req_lkid` and refill `out` from it, exactly
 * as the original post was filled -- same one function, same fields, same
 * moment-of-truth discipline (dlm_proxy_fill_post).
 *
 * THIS IS THE ANTI-LARP PRIMITIVE, and it is the whole reason the requester FSM
 * can be honest about retries. Every frame the FSM sends -- the first
 * transmission, a retransmit after a timeout, a retry at a new target after a
 * decline, and above all the COMPLETION that names the master's handle -- is
 * built from a FRESH call to this function, never from a field remembered off
 * an earlier frame or off the reply that arrived in between. A completion
 * carrying a lock id that came from a frame instead of from the lock database
 * is precisely what bugchecked a real VAX with INVLOCKID and took the cluster
 * down (commit fc8540ae).
 *
 * `op` and `dst_csid` are the caller's routing decision for THIS transmission
 * (the FSM knows whether it is re-sending an ENQ or sending a DEQ, and to whom);
 * every other field in `*out` comes out of the lock database.
 *
 * Returns SS$_NORMAL, or SS$_IVLOCKID when `req_lkid` names no proxy LKB of
 * this node's -- which is the honest answer when the lock was released or run
 * down while a request for it was outstanding, and the FSM must then abandon
 * the transmission rather than send a frame about a lock that no longer exists.
 */
uint32_t vms_lock_dlm_proxy_refill_post(uint32_t req_lkid, uint32_t op,
					uint32_t dst_csid,
					struct vms_dlm_proxy_post *out);

/*
 * END the wait on a proxy LKB with a real terminal status, because no answer is
 * coming: the target left the cluster, or the retransmit budget is spent.
 *
 * The same mechanism the departure path already uses (`grant_state` set under
 * res->lock, then the LKB's condition variable broadcast), so a $ENQW asleep in
 * enq_wait_sync wakes and returns `status` to its caller. A proxy that is
 * already GRANTED is left alone -- it is a real lock, and a real lock is not
 * something a timeout may take away.
 *
 * Returns SS$_NORMAL when a pending proxy was really failed, SS$_IVLOCKID when
 * the handle names no proxy, SS$_NORMAL with nothing done when it was already
 * granted. Never fabricates a completion.
 */
uint32_t vms_lock_dlm_proxy_fail(uint32_t req_lkid, uint32_t status);

/*
 * OUTCOME 3 (Davis p. 6-31): the DIRECTORY node answered "there is no master --
 * YOU master it". Record this node as the master of `resnam`'s tree and PROMOTE
 * the proxy LKB `req_lkid` into a real local lock, then run the local granting
 * algorithm over the resource.
 *
 * WHY PROMOTION IS THE CORRECT ACT AND NOT A SHORTCUT. Once the cluster has
 * told this node it masters the tree, this node's own granted/waiting queues
 * ARE the authority for it -- there is no other holder to consult, which is
 * exactly what "no master" meant. So the request is moved off res->proxies onto
 * res->waiting and try_grant_waiters() decides it, the same code path that
 * decides every local $ENQ. Nothing is granted that the local queues do not
 * allow, and the requester's $ENQW wakes from its own genuine grant.
 *
 * Returns SS$_NORMAL when the proxy was promoted (granted or genuinely queued),
 * SS$_IVLOCKID when the handle names no proxy of ours, SS$_BADPARAM when the
 * handle names a proxy on a different resource than `resnam` -- a mismatch that
 * would mean acting on an answer about some other lock.
 */
uint32_t vms_lock_dlm_assume_mastery(const char *resnam, uint32_t req_lkid);

/*
 * OUTCOME 2 (Davis p. 6-31): the directory node answered "the master is X".
 *
 * Record X on the proxy LKB `req_lkid` and on its resource block. Nothing is
 * granted, nothing is woken and no mode changes -- this is a ROUTING fact the
 * cluster returned, and the only thing it settles is where the request goes
 * next (p. 6-32: the RSB's master CSID is why the NEXT $ENQ in this tree goes
 * straight to the master instead of back through the directory).
 *
 * WHY THE WIRE ARM MUST NOT JUST REMEMBER IT. The retry is built from a FRESH
 * vms_lock_dlm_proxy_refill_post(), and that reads `lock->master_csid`. Putting
 * the answer INTO the lock database first is what makes the retry's destination
 * -- and its `to_directory` flag, and its master_csid field -- an executive
 * read rather than a value the FSM carried across two frames. An FSM that kept
 * the CSID in its own request block and addressed the retry from there would be
 * plumbing a wire value frame-to-frame, which is the exact anti-pattern INV-6
 * exists to stop.
 *
 * `resnam` must name the resource the proxy is on: acting on a directory reply
 * about one resource against a proxy on another would route the wrong tree.
 *
 * Returns SS$_NORMAL, SS$_IVLOCKID (no proxy of ours by that handle),
 * SS$_BADPARAM (null/empty name, zero CSID, or a name that does not match the
 * proxy's resource). A `master_csid` of 0 is refused: "unmastered" is what 0
 * MEANS in this engine, so it is not an answer.
 */
uint32_t vms_lock_dlm_record_master(const char *resnam, uint32_t req_lkid,
				    uint32_t master_csid);

/*
 * WHAT A GRANT TOLD US -- the kernel-core-typed door onto the engine's
 * requester-side grant receive (FC-P4.6).
 *
 * The engine already has that path (vms_lock_dlm_xnode_grant_recv): it records
 * the master's handle, the master's CSID, the granted mode and the master's
 * value block on the proxy LKB, and wakes a $ENQW asleep on it. But it is
 * reached through `struct vms_dlm_xnode_args` -- an IOCTL ABI struct that lives
 * in the per-substrate vms_ioctl.h twin, which a PURE kernel-core FSM may not
 * include (design §3.9; the include gate's RULE 2). So the wire arm reaches it
 * through this struct instead, and vms_lock.c -- the one TU that sees both --
 * does the translation.
 *
 * EVERY FIELD HERE IS SOURCED, AND `valblk_present` IS WHY IT NEEDS SAYING.
 *   req_lkid      OUR own handle, echoed by the master (codec body[20:24]);
 *                 the (req_csid, req_lkid) key that makes a retransmitted
 *                 grant find the SAME lock instead of minting a second.
 *   master_lkid   the master's handle (codec body[24:28]).
 *   master_csid   the SENDER's cluster-logical address, as the connection
 *                 manager identified the frame -- spec §4(a), the one
 *                 GROUNDED source for "who is the master" on a reply.
 *   granted_mode  codec body[30].
 *   valblk        the master's resource value block -- and there is NO
 *                 GROUNDED cat-0x02 field for it (vms_cluster_codec_dlm.h:
 *                 "the VALBLK round-trip was not exercised"). So
 *                 `valblk_present` is 0 on every frame this tree can parse
 *                 today, and the engine then leaves the proxy's own value
 *                 block ALONE. Writing sixteen zeros because the wire carried
 *                 nothing would be a fabricated value block -- INV-6's
 *                 "honest omission over a placeholder", in the one place where
 *                 the placeholder would be indistinguishable from data.
 *
 * Returns what the engine's grant receive returns: SS$_NORMAL, SS$_BADPARAM
 * (no handle, or a granted mode with no master handle -- the fc8540ae
 * refusal), SS$_IVLOCKID (the handle names a lock this node MASTERS, not a
 * proxy).
 */
struct vms_dlm_proxy_grant {
	uint32_t req_lkid;
	uint32_t master_lkid;
	uint32_t master_csid;
	uint8_t  granted_mode;
	uint8_t  valblk_present;
	uint8_t  pad[2];
	uint8_t  valblk[VMS_DLM_VALBLK_LEN];
};

uint32_t vms_lock_dlm_proxy_grant_recv(const struct vms_dlm_proxy_grant *g);

/*
 * A BLOCKING AST the master sent, for a lock THIS node holds through a proxy.
 *
 * The kernel-core-typed door onto vms_lock_dlm_xnode_blkast_recv, for the same
 * reason as the grant door above. The frame names OUR OWN handle, so the object
 * is found by a value this executive minted; the AST that fires is a REAL
 * user-mode AST queued to the process that owns the proxy, using the
 * blocking-AST routine THAT PROCESS supplied on its own $ENQ.
 *
 * Returns SS$_NORMAL when an AST was genuinely queued, and SS$_UNSUPPORTED --
 * never a faked delivery -- when the handle names no proxy of ours, when the
 * proxy carries no blocking-AST routine, or when it has no owning process (a
 * proxy the cluster reconstructed has none, and the cluster fork thread is not
 * a process to deliver to).
 */
uint32_t vms_lock_dlm_proxy_blkast_recv(uint32_t req_lkid);

#endif /* OVMX_VMS_DLM_PROXY_H */
