/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_dlm_dir.h - the LOCK DIRECTORY's entries: which system masters a
 * resource, for the resources whose directory node is THIS one (rd vms-8219;
 * plan row FC-P5.4).
 *
 * On VMS every resource tree is mastered on one node and found through its
 * DIRECTORY node, entry[(hash >> 16) mod n] of the Lock Directory Weight
 * Vector (vms_dlm_ldwv.h). A directory node holds, for each root resource
 * that maps to it, the CSID of the system that masters it (Davis pp. 6-31,
 * 6-50). With every member at the V7.3 default LOCKDIRWT 0, p. 6-32 makes
 * EVERY member a directory node -- OVMX included -- so a real VAX addresses
 * lookups, removals and rebuild registrations to an OVMX member.
 *
 * WHAT THIS OBJECT IS. A pure, fixed-capacity table keyed by the resource's
 * WIRE identity (group, access mode, name; vms_cluster_codec_dlm.h struct
 * vms_dlm_res_ident) with the master's CSID as the value -- and the one
 * decision a directory makes on a lookup (vms_dlm_dir_lookup). Storage is
 * the caller's; nothing here allocates, sleeps, logs or reads a clock, so
 * it is host-unit-testable as-is.
 *
 * ===========================================================================
 * THE TABLE IS KEYED BY THE NAME, NOT BY THE WIRE HASH (rd vms-025/db2a)
 *
 * It used to PROBE on `id->hash` -- the 32-bit value the asking system put on
 * the wire. That made the table unusable from the two directions this node's
 * own executive needs it from:
 *
 *   - THIS NODE'S OWN $ENQ. The lock engine holds a resource NAME and (for a
 *     name no system has looked up here yet) no hash at all. Probing on a hash
 *     it does not have means it cannot ask the question "does my own lock
 *     directory already name a master for this name?" -- and the measured
 *     consequence was two masters for one resource: a real VAX recorded
 *     master=VAX here, and this node's next $ENQ mastered the same name
 *     locally without ever looking.
 *   - AN ENTRY THIS NODE RECORDED ITSELF. A directory entry naming THIS node
 *     as master is created when this node masters the name, and this node has
 *     NO wire hash for its own first-touched names (computing one is
 *     Rule-8-forbidden). Filed under a hash of 0 it would be invisible to the
 *     VAX's later lookup -- which carries the VAX's real hash and would probe a
 *     different chain -- and that lookup would then be answered "you master
 *     it": the same two-master hole from the other side.
 *
 * So the probe index is a LOCAL index over the wire IDENTITY (group, access
 * mode, name bytes) and the match is by that identity, exactly as the book
 * describes a real directory node's own serving algorithm -- "scans that chain
 * BY NAME" (Davis p. 6-50). The received hash is not needed to index a PRIVATE
 * table (docs/design-cluster-book-grounding.md §1.2: "since OVMX's own table is
 * private, any local index"), and the index is OVMX's own well-understood
 * function of the name bytes, not a reproduction of DEC's unpublished directory
 * hash (Rule 8). It has no relationship to routing: the LDWV is indexed by the
 * WIRE hash and nothing here ever indexes anything with it.
 *
 * THE HASH IS STILL STORED, because it is the value this node may place on an
 * outbound frame for that resource (`hash_known`, Davis p. 6-50) -- either the
 * one the cluster put on the wire for it or the one this executive computed for
 * it (rd vms-b5b0). An entry carrying neither reads `hash_known == 0`: an honest
 * absence, never a zero dressed as a hash (the "honest 0" that made a real VAX
 * install OVMX as master of resources it did not master).
 *
 * AND THE MATCH IS NOW ALWAYS EXACT (rd vms-b5b0). The second bullet above is
 * historical: the engine carries a resource's UIC group and access mode on the
 * resource block itself, so a self-claim is recorded under the same full
 * identity a registration is, and the name-only entries -- with their
 * deliberately-argued conflation -- are gone.
 * ===========================================================================
 *
 * INV-6. Every entry is a fact the cluster put on the wire: a registration
 * from the master itself (op-0x0d), or a lookup this directory answered
 * "you master it" (and therefore MADE true). A full table is refused, never
 * answered -- answering "you master it" without recording it would let the
 * next asker become a second master.
 *
 * INCLUDES: kernel-core headers only.
 */
#ifndef OVMX_VMS_DLM_DIR_H
#define OVMX_VMS_DLM_DIR_H

#include "vms_cluster.h"            /* vms_csid_t */
#include "vms_cluster_codec_dlm.h"  /* struct vms_dlm_res_ident */

#ifdef __cplusplus
extern "C" {
#endif

/* Capacity of the production table: a power of two. An idle three-node V7.3
 * cluster put 939 root resources on the wire in minutes; with n = 3 a node
 * directs about a third of them. */
#define VMS_DLM_DIR_CAP 4096u

enum vms_dlm_dir_slot_state {
	VMS_DLM_DIR_EMPTY = 0,
	VMS_DLM_DIR_USED  = 1
};

struct vms_dlm_dir_entry {
	struct vms_dlm_res_ident id;
	vms_csid_t master;
	uint8_t    state;
	/*
	 * Does `id.hash` carry a value this executive genuinely holds for this
	 * resource -- one some system in this cluster put on the wire for it, or
	 * one computed for it by the PROVEN resource-name hash (rd vms-b5b0)? 0
	 * means this entry carries no usable value, and a caller that needs one
	 * to address a frame is told so rather than handed a zero (INV-6).
	 */
	uint8_t    hash_known;
	/*
	 * NAME-ONLY ENTRIES ARE GONE (rd vms-b5b0). This entry used to be able to
	 * carry a NAME and nothing else -- a self-claim recorded when the lock
	 * engine held a resource name but no UIC group or access mode -- and every
	 * matching rule in this file had to special-case it, with the conflation
	 * argued safe rather than absent. The engine now carries the full identity
	 * on every resource block, so every entry here has a group, an access mode
	 * and (for a self-claim too) a hash: one matching rule, EXACT IDENTITY, in
	 * both directions.
	 */
	uint8_t    pad[2];
};

struct vms_dlm_dir {
	struct vms_dlm_dir_entry *slot;
	uint32_t cap;              /* power of two; 0 = no table */
	uint32_t used;

	/* Counted facts. */
	uint32_t registered;       /* entries set from a master's registration */
	uint32_t answered_you;     /* lookups answered "you master it"         */
	uint32_t answered_redirect;/* lookups answered "the master is X"       */
	uint32_t removed;          /* entries removed by their master          */
	uint32_t remove_unknown;   /* a removal for no entry, or another master*/
	uint32_t full_refusals;    /* a new entry that did not fit             */
	uint32_t dropped;          /* entries dropped by a transition          */

	/* rd vms-025 / vms-db2a. */
	uint32_t self_claims;      /* entries THIS node recorded for its own   */
				   /* mastery of a name it touched first       */
	uint32_t answered_self;    /* lookups answered from a self-claim: this */
				   /* node masters the name, so the directory  */
				   /* resolves the request (Davis p. 6-51)     */
	uint32_t own_lookups;      /* this executive's own $ENQ consulted this */
				   /* directory about a resource it holds      */
};

enum vms_dlm_dir_outcome {
	VMS_DLM_DIR_ANSWER_YOU      = 0,   /* body[34] 0xf9: requester masters */
	VMS_DLM_DIR_ANSWER_REDIRECT = 1,   /* body[34] 0xf8: master is *out    */
	VMS_DLM_DIR_ANSWER_NONE     = 2,   /* table full / no table: refuse     */
	/*
	 * THIS NODE masters the name (Davis p. 6-31 outcome (a) / p. 6-51: "the
	 * directory node is itself master and resolves the request"). NOT a
	 * redirect naming ourselves: a redirect to self would send the asker
	 * straight back here with the identical frame, forever. The caller must
	 * serve the request AS THE MASTER or not answer it at all.
	 */
	VMS_DLM_DIR_ANSWER_SELF     = 3
};

/* `storage` holds `cap` entries; cap must be a power of two. 0 on success. */
int vms_dlm_dir_init(struct vms_dlm_dir *d, struct vms_dlm_dir_entry *storage,
		     uint32_t cap);

/* The entry for `id`, or NULL. */
const struct vms_dlm_dir_entry *vms_dlm_dir_find(const struct vms_dlm_dir *d,
						 const struct vms_dlm_res_ident *id);

/* A master registers a resource (op-0x0d): set or overwrite. 0 on success,
 * -1 when the table is full. */
int vms_dlm_dir_register(struct vms_dlm_dir *d,
			 const struct vms_dlm_res_ident *id, vms_csid_t master);

/*
 * THE LOOKUP (p. 6-31 outcomes 2 and 3). An entry naming another system ->
 * REDIRECT with *out_master. No entry -> the requester becomes the master:
 * recorded, then YOU. An entry naming the requester itself -> YOU. A full
 * table -> NONE (nothing recorded, nothing to answer).
 *
 * `self` is THIS node's own CSID when it is known, else 0. An entry naming it --
 * including a NAME-ONLY self-claim this node recorded for its own mastery
 * (vms_dlm_dir_claim_self) -- yields ANSWER_SELF: p. 6-51's outcome (a), where
 * the directory node is itself the master and resolves the request. Passing 0
 * disables that outcome entirely, which is the honest reading of "this node does
 * not know its own cluster identity".
 */
enum vms_dlm_dir_outcome vms_dlm_dir_lookup(struct vms_dlm_dir *d,
					    const struct vms_dlm_res_ident *id,
					    vms_csid_t requester,
					    vms_csid_t self,
					    vms_csid_t *out_master);

/*
 * THIS NODE IS ABOUT TO MASTER THIS RESOURCE -- record it in its own directory
 * (rd vms-db2a; Davis p. 6-51, "a directory node becomes master when it
 * acquires the first lock in the cluster on the root resource", whose directory
 * entry then names itself).
 *
 * Recorded under the FULL identity the engine holds (rd vms-b5b0): the name, the
 * UIC group, the access mode, and the directory hash -- learned off the wire or
 * computed by the proven function (vms_dlm_hash.h), whichever the resource block
 * carries. That is what makes a real VAX's lookup for the same NAME in a
 * DIFFERENT resource domain miss this entry and be answered on its own merits,
 * instead of being told about a master that is not its resource's. `id->hash` 0
 * still records `hash_known` 0 -- "no value is held" and "the value is 0" remain
 * different facts.
 *
 * Idempotent: a claim for a resource this node already claims changes nothing.
 * 0 when the claim is held afterwards, -1 when the table could not take it
 * (counted in `full_refusals`) or `master` is 0 -- and then the caller must NOT
 * proceed as if the directory named it, because the next asker would be told
 * "you master it".
 */
int vms_dlm_dir_claim_self(struct vms_dlm_dir *d,
			   const struct vms_dlm_res_ident *id,
			   vms_csid_t master);

/* What this node's own directory says about one ROOT RESOURCE. */
enum vms_dlm_dir_ident_outcome {
	VMS_DLM_DIR_IDENT_MASTER = 0,  /* an entry names a master: *out_master */
	VMS_DLM_DIR_IDENT_NONE,        /* no entry for that resource at all    */
	VMS_DLM_DIR_IDENT_INVAL        /* no table, or a null/empty identity   */
};

/*
 * ASK THIS NODE'S OWN DIRECTORY ABOUT A ROOT RESOURCE (rd vms-025) -- the
 * question the lock engine must ask before it masters anything, and could not
 * ask while the table was probed by a hash the engine does not hold.
 *
 * Matched by EXACT IDENTITY (name, UIC group, access mode) -- the same rule the
 * wire-facing ops match by, now that the engine holds an identity for every
 * resource (rd vms-b5b0). The predecessor matched by NAME ALONE across domains
 * and had to refuse an AMBIGUOUS answer when two domains of one name named
 * different masters; that case cannot arise against an exact key, so the
 * outcome is gone rather than left unreachable.
 *
 * `*out_hash`/`*out_hash_known` report the hash stored with the answering entry,
 * so the caller can place THAT value -- and only that value -- on a frame
 * addressed to the master it just learned. Both are optional.
 *
 * Takes no `claim`: recording is vms_dlm_dir_claim_self above, so a read of this
 * directory can never be a write by accident.
 */
enum vms_dlm_dir_ident_outcome
vms_dlm_dir_lookup_ident(struct vms_dlm_dir *d,
			 const struct vms_dlm_res_ident *id,
			 vms_csid_t *out_master, uint32_t *out_hash,
			 uint8_t *out_hash_known);

/* The master removes its entry (op-0x04). Only the master named by the entry
 * may remove it. 0 when removed, -1 otherwise (counted). */
int vms_dlm_dir_remove(struct vms_dlm_dir *d,
		       const struct vms_dlm_res_ident *id, vms_csid_t master);

/* A member left: every entry it mastered is gone with it. Returns how many. */
uint32_t vms_dlm_dir_drop_master(struct vms_dlm_dir *d, vms_csid_t master);

/*
 * The vector changed: drop every entry `keep` says this node no longer directs.
 * Returns how many.
 *
 * `hash_known` is 0 for an entry this node recorded for its own mastery, and
 * then `hash` carries NOTHING -- the predicate cannot be asked "does the vector
 * direct this hash here" about it and must decide on the vector's own shape
 * instead (in the sole-directory configuration every name is directed here, so
 * such an entry stays; otherwise it cannot be judged and goes, and its master
 * re-registers it with whatever node now directs it -- p. 6-33).
 */
uint32_t vms_dlm_dir_drop_unless(struct vms_dlm_dir *d,
				 int (*keep)(void *ctx, uint32_t hash,
					     int hash_known),
				 void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_DLM_DIR_H */
