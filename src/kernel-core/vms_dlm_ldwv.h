/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_dlm_ldwv.h - the Lock Directory Weight Vector and `dir_resolve`: how a
 * ROOT resource name is routed to its DIRECTORY NODE (FC-P4.3).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.6 D-DLM-2 (the resolved
 * A/A'/B/C ladder). Grounding: docs/research-dlm-directory-algorithm.md
 * (FC-P4.1), from Roy G. Davis, *VAXcluster Principles* (Digital Press, 1993),
 * ch. 6/7 -- page cites only, clean-room (CLAUDE.md Rule 8).
 *
 * ===========================================================================
 * THE TWO HALVES, AND WHY ONLY ONE OF THEM IS COMPUTED HERE
 *
 * A directory lookup needs two things: a 16-bit HASH of the root resource
 * name, and a VECTOR to index with it.
 *
 *   THE VECTOR IS PUBLISHED, so OVMX builds it (rung A). Entries per system =
 *   that system's LOCKDIRWT; one entry per system when every LOCKDIRWT is 0;
 *   a system's entries are contiguous; every member's copy is logically
 *   equivalent and differs only in that its OWN entries read 0; the entry at
 *   `hash mod n` names the directory node (pp. 6-31/6-32, Fig. 6-18 p. 6-33).
 *   It is resized in Phase 1 and filled at Phase 2 of a state transition,
 *   before the synchronised rebuild (pp. 7-40..7-42), and every change
 *   discards all cached directory information (p. 6-33).
 *
 *   THE HASH FUNCTION IS NOT PUBLISHED AT THE BIT LEVEL, so OVMX NEVER
 *   COMPUTES IT (rung A'). It does not have to: p. 6-50 documents that every
 *   directory lookup carries the sending system's own 16-bit hash value on the
 *   wire, and that the directory node USES the received value to index its
 *   Resource Hash Table. So the value is READ OFF THE WIRE and learned into
 *   the resource block (`hash16`/`hash_known`, vms_lock.c), and a lookup is
 *   never sent with a hash this node did not receive.
 *
 * WHY THAT SECOND HALF IS NOT PEDANTRY. A lookup carrying a placeholder hash
 * makes the directory node scan the WRONG chain, fail to find the name, and
 * take outcome (3) of p. 6-31: it creates a directory entry naming the SENDER
 * as the master. The campaign's 35-per-second "grant storm" against a real
 * VAX -- 48 resources "granted", then re-requested forever -- is exactly that,
 * caused by an "honest 0" in this field (memory cluster-promotion-gap;
 * research note SS3). Sending a placeholder hash does not fail locally, it
 * silently corrupts the peer's lock directory. Hence: wire-learned or refuse.
 *
 * ===========================================================================
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_DLM_LDWV_H
#define OVMX_VMS_DLM_LDWV_H

#include "vms_cluster.h"   /* struct vms_ldwv / vms_club / vms_csb */
#include "vms_cnxman.h"    /* struct cnxman_ops -- the injected log */

/* ==========================================================================
 * 1. Results
 *
 * Deliberately NOT SS$_ codes: this TU is pure and compiles on the host with no
 * vms_internal.h in scope (design SS3.9, test-ladder rung 1). The lock engine
 * maps them to SS$_ at its own boundary, which is where the executive's status
 * vocabulary lives.
 * ========================================================================== */
enum vms_ldwv_status {
	VMS_LDWV_OK          = 0,
	VMS_LDWV_E_INVAL     = 1,  /* a null argument                          */
	VMS_LDWV_E_NOVEC     = 2,  /* no vector, or not authoritative right now */
	VMS_LDWV_E_TOOBIG    = 3,  /* the weighted set exceeds the storage bound*/
	VMS_LDWV_E_WEIGHTS   = 4,  /* some LOCKDIRWTs learned, some not         */
	VMS_LDWV_E_NOMEMBERS = 5   /* no selected member with a learned CSID    */
};

/* ==========================================================================
 * 2. One system's contribution to the vector
 *
 * The builder's input, one per SELECTED member. Every field is a value the
 * connection manager LEARNED: the CSID from a membership record, the LOCKDIRWT
 * from that system's parameters record. `lockdirwt_valid` is 0 when the system
 * has not advertised one -- not "its weight is zero" (INV-6).
 * ========================================================================== */
struct vms_ldwv_member {
	vms_csid_t csid;             /* LEARNED; never 0 for a real member  */
	uint8_t    lockdirwt;        /* LEARNED, meaningful only if valid   */
	uint8_t    lockdirwt_valid;
	uint8_t    is_local;         /* this system: its entries read 0     */
	uint8_t    pad;
};

/* ==========================================================================
 * 3. Building it
 * ========================================================================== */

/* Zero a vector to "no vector at all" (generation 0). */
void vms_ldwv_init(struct vms_ldwv *v);

/*
 * PHASE 1 (p. 7-40/7-41): a transition that can change the vector has opened.
 * The vector stops being authoritative here and stays that way until Phase 2
 * refills it -- p. 6-33's "all directory information is discarded". The
 * generation is bumped, which is what invalidates every `rsb->dir_csid` the
 * lock engine cached against the old vector (SS4 below). Idempotent in effect,
 * but NOT in generation: two invalidations are two changes.
 */
void vms_ldwv_invalidate(struct vms_ldwv *v);

/*
 * PHASE 2 (p. 7-42): fill the vector from the committed membership.
 *
 * `m[0..n_members)` must already be in the order the members occupy the
 * vector -- see vms_ldwv_build_from_club(), which is what establishes that
 * order from real CSBs. Lays down `lockdirwt` contiguous entries per member,
 * or exactly one per member when NO member has a nonzero weight (p. 6-32),
 * writing 0 for the local system's own entries (p. 6-32, Fig. 6-18).
 *
 * REFUSES rather than approximating:
 *   VMS_LDWV_E_WEIGHTS   some members advertised a LOCKDIRWT and others did
 *                        not. A vector built from a mixture puts every entry
 *                        after the unknown member at the wrong offset, i.e.
 *                        disagrees with every other member's copy.
 *   VMS_LDWV_E_TOOBIG    the weighted set exceeds VMS_LDWV_MAX_ENTRIES.
 *   VMS_LDWV_E_NOMEMBERS nobody to put in it.
 * On any refusal the vector is left INVALID (and the generation bumped), never
 * partially filled.
 */
enum vms_ldwv_status vms_ldwv_build(struct vms_ldwv *v,
				    const struct vms_ldwv_member *m,
				    uint32_t n_members);

/*
 * The p. 6-32 entry count for a member set, as vms_ldwv_build would lay it
 * down. Exposed because Phase 1's size adjustment and Phase 2's fill must
 * agree, and because a test can then assert the published worked example
 * (p. 6-33: weights 1 and 3 over four systems -> four entries) directly.
 * Returns 0 when the set is unusable; `*all_zero` (optional) reports whether
 * the one-entry-per-system rule applied.
 */
uint32_t vms_ldwv_entry_count(const struct vms_ldwv_member *m,
			      uint32_t n_members, int *all_zero);

/* ==========================================================================
 * 4. Resolving -- the index rule (p. 6-31)
 *
 * `vms_ldwv_resolve` is `dir_resolve`. It takes the hash as a PARAMETER
 * because the hash is not this layer's to produce: the caller must have
 * learned it from the wire (vms_lock.c's `rsb->hash16`/`hash_known`). There is
 * deliberately no overload of these functions that takes a resource NAME --
 * an API that accepted a name would be an API that invited someone to hash it.
 *
 * `*out_csid` is written ONLY on VMS_LDWV_OK, and 0 there means THIS NODE is
 * the directory (p. 6-32: a system's own entries read 0 in its own copy).
 *
 * `vms_ldwv_generation` is the cache key: an engine that cached a resolution
 * re-resolves as soon as this value changes, so a resolution can never survive
 * the vector it was computed from.
 * ========================================================================== */
enum vms_ldwv_status vms_ldwv_index(const struct vms_ldwv *v, uint16_t hash16,
				    uint32_t *out_index);

enum vms_ldwv_status vms_ldwv_resolve(const struct vms_ldwv *v, uint16_t hash16,
				      vms_csid_t *out_csid);

uint32_t vms_ldwv_generation(const struct vms_ldwv *v);

/* Nonzero iff `hash16` indexes one of THIS node's own entries -- i.e. iff this
 * node is the directory for that hash. The directory-node role's admission
 * test (p. 6-31) and the order self-check's predicate (SS5). */
int vms_ldwv_is_ours(const struct vms_ldwv *v, uint16_t hash16);

/* ==========================================================================
 * 5. The CLUB-facing half
 * ========================================================================== */

/*
 * Build the CLUB's vector from its own CSBs -- the Phase 2 fill. Members are
 * the CSBs carrying VMS_CSB_F_SELECTED with a LEARNED CSID, laid down in
 * CLUSTER SYSTEM VECTOR INDEX ORDER: ascending (CSID & 0xffff), the CSV slot
 * the low half of a CSID names (p. 7-25).
 *
 * THE ORDER IS THE ONE RESIDUAL AND IT IS SELF-CHECKING. The book fixes
 * contiguity and cluster-wide agreement of offsets but does not state the
 * layout order; CSV-index order is the natural candidate and the one the
 * p. 6-33 figure is consistent with (research note SS1). It is falsifiable
 * without a capture: see cnxman_dir_lookup_received() below.
 *
 * On refusal the CLUB's `ldwv_build_refused` counter rises, one %CNXMAN line
 * is emitted through `ops`, and the vector is left invalid. Returns the
 * vms_ldwv_build status.
 */
enum vms_ldwv_status cnxman_ldwv_rebuild(struct vms_club *club,
					 const struct cnxman_ops *ops);

/*
 * THE ORDER SELF-CHECK. Call once for every directory lookup this node
 * RECEIVES, with the hash the sender put on the wire and the sender's CSID.
 *
 * Every such lookup must index one of this node's own entries, because the
 * sender resolved it through a copy of the vector that is logically equivalent
 * to ours (p. 6-32). If it does not, one of two things is true and both are
 * worth knowing: our layout ORDER hypothesis is wrong, or our vector is stale
 * relative to the sender's. Either way the CLUB's `dir_lookup_misaddressed`
 * rises and a %CNXMAN line names the sender -- the lookup is NOT refused here
 * (refusing is a protocol decision that belongs to the directory-node role,
 * FC-P5.4), it is counted so the hypothesis can be falsified from real traffic
 * instead of from a guess.
 *
 * Returns nonzero iff the lookup was correctly addressed to this node.
 */
int cnxman_dir_lookup_received(struct vms_club *club, uint16_t hash16,
			       vms_csid_t from_csid, const struct cnxman_ops *ops);

#endif /* OVMX_VMS_DLM_LDWV_H */
