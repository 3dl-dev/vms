/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_dlm_rm_fsm.h - THE NEW MASTER'S SIDE OF A LOCK-TREE REMASTER (rd vms-ed5,
 * under rd vms-50d). Design docs/design-faithful-cluster-executive.md SS3.6
 * (D-DLM-4/D-DLM-5, "remaster on departure"); wire shape and every cell
 * vms_cluster_codec_dlm.h "LOCK-TREE REMASTERING ON THE WIRE" (rd vms-380).
 *
 * ===========================================================================
 * WHAT THIS IS
 * ===========================================================================
 * A real VMS node hands one of its lock trees to this node. Three pieces
 * already existed and were already proven; this file is the only thing missing
 * between them, and it owns no lock state of its own:
 *
 *   the ADOPTION ENGINE   vms_dlm_adopt.h -- vms_lock_dlm_adopt_open/_lock/
 *                         _end/_abort. It creates the REAL master-side RSBs and
 *                         LKBs, mints the handles, and is the ONLY thing here
 *                         that holds anything. Injected through `ops` so this
 *                         file is pure.
 *   the CODEC             vms_cluster_codec_dlm.h -- the parsers for the five
 *                         inbound steps, the two answer builders and the
 *                         op-0x14 builder. Linked directly, like the requester
 *                         FSM links it: the bytes are the ones 662 of 662 real
 *                         answers pinned byte for byte.
 *   the CONNECTION MGR    vms_cnxman.c -- the CSBs (who the other members ARE),
 *                         the envelope, and the one way out. Injected.
 *
 * So: this file is a TABLE. Five inbound steps and a departure drive one
 * per-tree state machine; every value it puts on the wire comes out of the
 * engine or is the request echoed back by the codec's own grounded builder.
 *
 * ===========================================================================
 * THE SEQUENCE, AND THE TWO RULES THAT ARE *THIS FILE'S* GROUNDING
 * ===========================================================================
 * The codec header has the per-cell evidence. What this file adds is WHEN:
 * measured over every pcap in tests/lab/captures (123 captures, 61 remaster
 * episodes that reach an op-0x14, 68 op-0x13 and 68 op-0x14 frames; the
 * measurement is tools/cluster/remaster_census.py --roles, whose figures
 * `--selftest` pins):
 *
 *   O -> N   0x0e, 0x0f, 0x10      the hand-over opens; each answered 82/15
 *   O -> M   0x0f, 0x11            every other member is told
 *   X -> N   0x12 x k              one record per lock X holds in the tree,
 *                                  ROOT first; each answered 82/01
 *   X -> N   0x13                  "that is every record from me"
 *   N -> X   0x14                  "the tree is held here"
 *
 * RULE W1 -- ONE op-0x14 PER op-0x13, TO ITS SENDER, AND TO NOBODY ELSE.
 * In 61 of 61 episodes the SET of systems that sent the new master an op-0x13
 * is EXACTLY the set of systems it sent an op-0x14 to. Not once does a 0x14 go
 * to a system that sent no 0x13 (0 of 68).
 *
 * RULE W2 -- THE op-0x14s GO OUT ONLY AFTER THE LAST op-0x13 IS IN.
 * In all 7 episodes with more than one op-0x13, every op-0x14 follows every
 * op-0x13 (0 of 7 interleave). The naive "answer each 0x13 as it lands" is
 * therefore NOT what a real new master does -- and it is also unsafe: in the
 * pre-registered RA8 arm (tests/lab/captures/vms-380-ra8-heldout-20261021)
 * the OLD MASTER's 0x13 arrives BEFORE the holder's four 0x12 records, so a
 * node that treated the first 0x13 as the end would have closed the adoption
 * and then refused four locks a real VAX still believes it holds.
 *
 * WHO IS OWED ONE, then? Every OTHER MEMBER OF THE CLUSTER. In every episode
 * the op-0x13 senders are exactly the SELECTED members other than the new
 * master -- including, in the RA8 CACHE$ episode, a member that held nothing
 * in the tree and sent no record at all. That set is not on the wire this node
 * can see (the 0x0f and 0x11 that tell the others are point-to-point), so it
 * is READ FROM THE CLUB (`ops->other_members`, p. 7-49's SELECTED set minus
 * this node) when the 0x0e opens the tree, and a member that LEAVES mid-tree
 * is struck from it (`vms_dlm_rm_member_departed`) rather than waited for
 * forever.
 *
 * ===========================================================================
 * INV-6, STRUCTURALLY: WHAT THIS FILE CAN AND CANNOT ASSERT
 * ===========================================================================
 * 1. `struct vms_dlm_rm_tree` holds NO lock id, NO mode, NO value block and NO
 *    hash. It physically cannot carry one from an inbound frame to an outbound
 *    one. What it holds is the tree's IDENTITY (which resource), the system
 *    that is handing it over, and who still owes an end-of-records.
 * 2. The 82/01 answer to a 0x12 is built from `struct
 *    vms_dlm_adopt_lock_result` -- the engine's read-back of the LKB and RSB it
 *    just created. The handle in it is the handle the engine MINTED; a record
 *    the engine refuses is NOT answered.
 * 3. The 82/15 answer to a 0x0e / 0x0f / 0x10 is the request echoed with three
 *    bytes rewritten, by the codec's one builder. It asserts no value.
 * 4. The op-0x14 is sent only after `ops->adopt_end` has returned SS$_NORMAL
 *    for that exact tree -- which it does only when an adoption of that root
 *    resource was really open here -- AND `ops->name_mastered_here` confirms
 *    this executive now masters the name. Two independent executive reads
 *    behind one statement ("the tree is held here"). Its directory hash comes
 *    from `ops->dir_hash` (the value a system in this cluster put on the wire
 *    and the engine LEARNED into the RSB); unknown means body[128:132] stays
 *    zero, never a minted hash (Rule 8).
 * 5. A tree that cannot be adopted WHOLE is adopted NOT AT ALL:
 *    `ops->adopt_abort` releases exactly what this transaction installed, and
 *    nothing is answered or reported. A half-adopted tree served as master is
 *    a lock a holder believes it holds and this node does not have.
 *
 * INCLUDES: kernel-core headers only (CI gate
 * tools/ci/cluster_core_includes_gate.sh RULE 1/2).
 */
#ifndef OVMX_VMS_DLM_RM_FSM_H
#define OVMX_VMS_DLM_RM_FSM_H

#include "vms_cluster_codec_cm.h"    /* VMS_CM_BODY_LEN, the CM's body size */
#include "vms_cluster_codec_dlm.h"   /* the steps, the builders, the idents */
#include "vms_dlm_adopt.h"           /* the engine seam this file drives    */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * HOW MANY TREES AT ONCE, AND WHERE THE NUMBER COMES FROM. Concurrent
 * remasters interleave on a real cluster, so one slot is not enough; the
 * corpus says how many. Over every capture in tests/lab/captures, the most
 * episodes one new master had open at the same moment -- a 0x0e seen and its
 * 0x14 not yet sent -- is FIVE (order1-vax-first.pcap, mount-type-over-mscp,
 * mscp-read-write: all of them a boot or a mount, the busiest remastering a
 * real cluster does). This is that worst case with room over it.
 *
 * It is a FIXED table on purpose: an unbounded list of trees is an unbounded
 * allocation driven by a peer's frames, in the executive. A 0x0e that finds no
 * free slot is REFUSED, COUNTED and SAID -- the honest "this node will not be
 * your new master", which leaves the tree with its old master, rather than a
 * tree adopted halfway.
 */
#define VMS_DLM_RM_TREES    32u

/*
 * HOW MANY MEMBERS ONE TREE CAN BE OWED AN END-OF-RECORDS BY. VMS's own
 * cluster limit is 96 members (VMS_CLUB_MAX_CSB is this executive's); this is
 * the width of the per-tree owed set, and a cluster wider than it refuses the
 * tree rather than wait for a member it cannot name.
 */
#define VMS_DLM_RM_MEMBERS  16u

/* ==========================================================================
 * THE TABLE'S AXES
 * ========================================================================== */

enum vms_dlm_rm_state {
	VMS_DLM_RM_ST_FREE = 0,   /* no tree here                              */
	VMS_DLM_RM_ST_OPEN,       /* the engine has an adoption open for it     */
	VMS_DLM_RM_ST_HELD,       /* every owed 0x13 in, adopted, 0x14s sent    */
	VMS_DLM_RM_ST__COUNT
};

enum vms_dlm_rm_event {
	VMS_DLM_RM_EV_OPEN = 0,   /* op 0x0e: "you are the new master of T"     */
	VMS_DLM_RM_EV_ROLE,       /* op 0x0f: told to every member              */
	VMS_DLM_RM_EV_PREP,       /* op 0x10: the third opening step            */
	VMS_DLM_RM_EV_RECORD,     /* op 0x12: one lock a holder holds in T      */
	VMS_DLM_RM_EV_END,        /* op 0x13: "that is every record from me"    */
	VMS_DLM_RM_EV__COUNT
};

/*
 * WHAT BECAME OF ONE MEMBER THIS TREE WAS WAITING ON. Only REPORTED earns an
 * op-0x14 (rule W1); DEPARTED stops the wait without making the departed system
 * a recipient, because a system that has left the cluster is not one a frame
 * may be addressed at.
 */
#define VMS_DLM_RM_OWED_WAITING   0u
#define VMS_DLM_RM_OWED_REPORTED  1u
#define VMS_DLM_RM_OWED_DEPARTED  2u

/* What became of one inbound step. */
enum vms_dlm_rm_status {
	VMS_DLM_RM_NOT_MINE = 0,  /* not a remaster step: the caller goes on    */
	VMS_DLM_RM_HANDLED,       /* acted on; an answer is staged if one is due */
	VMS_DLM_RM_REFUSED        /* a remaster step this node will not act on   */
};

/* ==========================================================================
 * THE INJECTED DOORS -- every one of them a read of, or a write to, something
 * that really exists. None of them may be NULL; a NULL door makes every step
 * REFUSED, which is the honest floor for a node that cannot reach its engine.
 * ========================================================================== */
struct vms_dlm_rm_ops {
	void *ctx;

	/*
	 * THE CLUB. The SELECTED members other than this node (p. 7-49), as
	 * the connection manager holds them. Returns how many CSIDs were
	 * written; > `max` is reported as `max` + 1 so the caller can refuse
	 * rather than silently wait for a member it did not record.
	 */
	uint32_t (*other_members)(void *ctx, uint32_t *csids, uint32_t max);

	/*
	 * THE ADOPTION ENGINE (vms_dlm_adopt.h), one door per entry point.
	 *
	 * Each returns 1 when the ENGINE REALLY DID IT and 0 when it refused.
	 * The SS$_ vocabulary deliberately does not cross this seam -- this TU
	 * holds no status literal, exactly as vms_cluster_sysgen.h holds none
	 * -- so the arm that binds these makes the one comparison and this
	 * table stays host-buildable with a plain compiler.
	 */
	int (*adopt_open)(void *ctx, const struct vms_dlm_adopt_tree *t);
	int (*adopt_lock)(void *ctx, const struct vms_dlm_adopt_tree *t,
			  const struct vms_dlm_adopt_lock_rec *r,
			  struct vms_dlm_adopt_lock_result *out);
	int (*adopt_end)(void *ctx, const struct vms_dlm_adopt_tree *t,
			 uint32_t *n_locks);
	int (*adopt_abort)(void *ctx, const struct vms_dlm_adopt_tree *t,
			   uint32_t *n_undone);
	/*
	 * WHICH TREE did this engine mint `master_lkid` inside? A 0x12 for a
	 * SUB-resource names its own resource, not the tree's root, so the only
	 * thing that places it in a tree is the PARENT handle at body[40:44] --
	 * a handle this engine minted and answered moments earlier. The ENGINE
	 * is asked, not a table kept here: a record naming a handle no open
	 * adoption minted gets 0 back and is refused.
	 */
	int (*adopt_tree_of_lkid)(void *ctx, uint32_t master_lkid,
				  struct vms_dlm_adopt_tree *out);
	/* The completed adoption really was reported (its 0x14s went out). */
	void (*adopt_reported)(void *ctx);

	/* Does this executive master that name NOW? (the 0x14's second read) */
	int (*name_mastered_here)(void *ctx, const char *resnam, uint16_t group,
				  uint8_t mode);
	/*
	 * The directory hash this executive LEARNED for the name, if any
	 * (vms_dlm_proxy.h: a value some system in this cluster put on the
	 * wire, never computed here). 0 = none held, and then the 0x14 carries
	 * zeros there.
	 */
	int (*dir_hash)(void *ctx, const char *resnam, uint16_t group,
			uint8_t mode, uint32_t *out_hash);

	/*
	 * STAGE the answer to the step being handled. RULE A (vms_dlm_scs.h):
	 * nothing here sends a reply -- the connection manager sends it,
	 * correlated with the transaction it answers.
	 */
	int (*stage_reply)(void *ctx, const uint8_t *body, uint32_t len);
	/* ORIGINATE one body at one member. The op-0x14's only way out. */
	int (*send)(void *ctx, uint32_t dst_csid, const uint8_t *body,
		    uint32_t len);

	/*
	 * rd vms-6ad1 (RA7): the byte written across the op-0x14's RESIDUE
	 * span. 0 is the vms-42de ruling's zeros and is what ships; RA7 fires
	 * the same remaster a second time with a non-zero pattern to prove the
	 * span is not read. A door rather than a field so the value is a
	 * CONFIGURATION this executive was given, never a literal in here.
	 */
	uint8_t (*residue_fill)(void *ctx);

	/* The one-line-per-fact console voice. */
	void (*log)(void *ctx, const char *msg);
};

/* ==========================================================================
 * ONE TREE BEING HANDED OVER
 * ========================================================================== */
struct vms_dlm_rm_tree {
	/* The tree, as the engine is asked about it. Set once, from the 0x0e. */
	struct vms_dlm_adopt_tree tree;
	/* Its WIRE identity -- group, mode, name -- for the op-0x14 it ends in.
	 * `hash` is deliberately NOT kept here: it is re-read from the engine
	 * when the 0x14 is built (INV-6 point 4). */
	struct vms_dlm_res_ident  id;

	/*
	 * THE MEMBERSHIP THIS HAND-OVER IS WAITING ON. `owed` is the CLUB's
	 * answer when the tree was handed over, kept as a record for the whole
	 * life of the slot; `done[i]` is what became of owed[i]. A member is
	 * never removed, so "announced to everyone who reported, and to nobody
	 * else" stays checkable after the fact.
	 */
	uint32_t owed[VMS_DLM_RM_MEMBERS];
	uint8_t  done[VMS_DLM_RM_MEMBERS];   /* VMS_DLM_RM_OWED_*            */
	uint8_t  n_owed;
	uint8_t  n_done;                     /* reported OR departed         */

	uint8_t  state;        /* enum vms_dlm_rm_state                       */
	uint8_t  pad;
	uint32_t records;      /* records the engine really installed for it   */
	uint32_t held_sent;    /* op-0x14s this tree really put on the wire    */
};

/* ==========================================================================
 * THE MACHINE. No globals, no clock, no allocation: the caller owns it.
 * ========================================================================== */
struct vms_dlm_rm {
	const struct vms_dlm_rm_ops *ops;
	struct vms_dlm_rm_tree tree[VMS_DLM_RM_TREES];

	/*
	 * The frame scratch. The codec's builders write FRAME-absolute, so this
	 * buffer carries the envelope span too and the body is the splice at
	 * VMS_OFF_SYSAP_BODY -- the codec's own published body origin, named
	 * here and nowhere else in this file.
	 */
	uint8_t txframe[VMS_OFF_SYSAP_BODY + VMS_CM_BODY_LEN];

	/* Counted facts. Every one of them is a thing that really happened. */
	uint32_t opens;              /* trees this node really opened          */
	uint32_t opens_refused;      /* ... and the ones it would not open     */
	uint32_t acks_sent;          /* 82/15 answers staged (0x0e/0x0f/0x10)  */
	uint32_t roles_as_new_master;/* 0x0f answered in the NEW MASTER role   */
	uint32_t records_adopted;    /* 0x12 records that became a real LKB    */
	uint32_t records_duplicate;  /* ... retransmits, same handle returned  */
	uint32_t records_refused;    /* ... refused: the tree was given back   */
	uint32_t ends_seen;          /* 0x13 end-of-records accepted           */
	uint32_t ends_unexpected;    /* 0x13 from a system owed nothing        */
	uint32_t trees_held;         /* adoptions this node really completed   */
	uint32_t held_sent;          /* op-0x14s really put on the wire        */
	uint32_t held_unsent;        /* op-0x14s owed and not sent (counted,   */
				     /* never faked -- see `send` failures)    */
	uint32_t held_not_mastered;  /* adopt_end said yes and the engine does */
				     /* not master the name: NOTHING sent      */
	uint32_t notifications;      /* op-0x11: another member is new master  */
	uint32_t aborts;             /* trees released after a refused record  */
	uint32_t departures_struck;  /* owed members struck off by a departure */
	uint32_t codec_failures;
	uint32_t no_slot;            /* a 0x0e with every tree slot busy       */
	uint32_t acks_received;      /* 82/15 answers to op-0x14s of ours      */
};

/* ==========================================================================
 * THE ENTRY POINTS
 * ========================================================================== */

/* Bind the doors. `ops` may be NULL only in a test that then expects every
 * step REFUSED. */
void vms_dlm_rm_init(struct vms_dlm_rm *f, const struct vms_dlm_rm_ops *ops);

/*
 * ONE INBOUND REMASTER STEP. `category`, `opcode` and `from_csid` are the
 * CONNECTION MANAGER's own reading of the frame it delivered -- the same
 * fields the rest of the arm routes on, and never a re-parse of the envelope
 * here; `body` is the 132-byte DLM body, which the codec re-checks against
 * them, so this table cannot be handed a body that is not what the envelope
 * claimed.
 *
 * VMS_DLM_RM_NOT_MINE for anything that is not one of the five steps, so the
 * caller's own routing is unchanged by this file's presence.
 */
enum vms_dlm_rm_status vms_dlm_rm_step(struct vms_dlm_rm *f, uint32_t from_csid,
				       uint8_t category, uint8_t opcode,
				       const uint8_t *body, uint32_t len);

/*
 * AN INBOUND cat-0x82 op-0x15 that may be the answer to an op-0x14 of ours.
 * Returns 1 when this node really is owed one by that system for a tree it
 * really holds (counted), 0 otherwise -- and then the caller's reply path sees
 * the frame unchanged. Nothing is emitted either way.
 */
int vms_dlm_rm_held_ack(struct vms_dlm_rm *f, uint32_t from_csid,
			uint8_t category, uint8_t opcode,
			const uint8_t *body, uint32_t len);

/* op-0x11: some OTHER member is the new master of a tree. A real VMS member
 * never answers one (7 of 7 carry no transaction), so this only counts it. */
void vms_dlm_rm_notified(struct vms_dlm_rm *f);

/*
 * A member LEFT. It is struck from every tree's owed set -- a departed system
 * will not be sending an end-of-records -- which may complete a tree that was
 * waiting only for it.
 */
void vms_dlm_rm_member_departed(struct vms_dlm_rm *f, uint32_t csid);

/* ==========================================================================
 * READBACKS -- what this machine really holds, for the arm and the tests
 * ========================================================================== */

/* Is a tree with this ROOT identity open here right now (the op-0x0f NEW
 * MASTER role)? `name_len` 0 or a NULL name is never a match. */
int vms_dlm_rm_is_new_master(const struct vms_dlm_rm *f,
			     const struct vms_dlm_res_ident *id);

/* The slot holding that root identity, or NULL. */
const struct vms_dlm_rm_tree *
vms_dlm_rm_find(const struct vms_dlm_rm *f, const struct vms_dlm_res_ident *id);

/* How many tree slots are in `st`. */
uint32_t vms_dlm_rm_trees_in_state(const struct vms_dlm_rm *f,
				   enum vms_dlm_rm_state st);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_DLM_RM_FSM_H */
