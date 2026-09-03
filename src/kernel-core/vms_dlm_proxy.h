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
 * INCLUDES: kernel-core headers only, plus the per-substrate vms_internal.h
 * twin every executive core TU includes (CI gate
 * tools/ci/cluster_core_includes_gate.sh RULE 1/2).
 */
#ifndef OVMX_VMS_DLM_PROXY_H
#define OVMX_VMS_DLM_PROXY_H

#include "vms_internal.h"   /* fixed-width types + LCK_VALBLK_SIZE */

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
	uint32_t op;           /* VMS_DLM_OP_ENQ (request/convert) or _DEQ      */
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
	uint8_t  valblk[LCK_VALBLK_SIZE];  /* the LKB's value block (a write
	                        * crossing carries it; otherwise zeros)         */
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

#endif /* OVMX_VMS_DLM_PROXY_H */
