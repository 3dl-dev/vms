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
 * WHO IS OWED ONE, then? NOT the whole club -- A MEMBER WITH NO ROLE IN THE
 * TREE SENDS NOTHING, AND IS NOT EVEN TOLD. That used to read "every other
 * member of the cluster", from a census of uncontrolled episodes; a member
 * that owes nothing then leaves the tree un-HELD forever, which is the double
 * grant of rd vms-5cd. It is now settled by a DRIVEN, PRE-REGISTERED
 * ONE-VARIABLE ARM on two real OpenVMS VAX V7.3 nodes and one OVMX node
 * (rd vms-083, tests/lab/captures/vms-083-dirnode-20261022): same three
 * members, same LOCKDIRWTs 1/1/0, same holder at the same mode, same
 * STOP-the-old-holder trigger, and ONE variable moved -- which node the tree's
 * root NAME assigns as its DIRECTORY node, set by reading the DIRENTR off the
 * real nodes' own SDA and verified on the wire. Four runs, both VAX role
 * assignments:
 *
 *   directory is a THIRD member  ->  TWO op-0x13s: the old master AND the
 *                                    directory node (which held NO lock and
 *                                    sent NO record); the third member was
 *                                    told by an op-0x0f and an op-0x11.
 *   directory IS the old master  ->  ONE op-0x13, from the old master alone.
 *                                    The remaining member got NO op-0x0f and
 *                                    NO op-0x11 and sent nothing -- 180 s.
 *
 * So the owed set is COMPUTED FROM EXECUTIVE STATE, as three things this node
 * genuinely holds, and never as a count or a majority:
 *
 *   1. THE OLD MASTER -- the system the 0x0e came FROM, as the connection
 *      manager identified it (`in->from_csid`). A tree whose old master this
 *      node cannot address is refused.
 *   2. THE TREE'S DIRECTORY NODE -- `ops->dir_node`, which resolves the hash
 *      the ENGINE holds for the root name through the connection manager's own
 *      weight vector (the same two reads the requester path routes a lookup
 *      with). Added only when it names a SELECTED member other than this node.
 *      Not knowable -> not added, and the arm's own evidence for that
 *      resolution being right is that OVMX's lookups for all four arm names
 *      went to the node the real cluster's SDA named as that name's directory.
 *   3. EVERY MEMBER THAT REALLY SENT A 0x12 RECORD -- a holder, added when its
 *      record is adopted, because a holder's 0x13 follows its records.
 *
 * And because the prediction may be incomplete where this node holds no hash,
 * a 0x13 from a SELECTED member that is not in the set is ADMITTED and acked
 * rather than refused (counted separately): rule W1 is about the senders,
 * which are a fact, where the owed set is a prediction. A member that LEAVES
 * mid-tree is struck from it (`vms_dlm_rm_member_departed`) rather than waited
 * for forever.
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
	/* `resnam_len` is the name's COUNTED length (rd vms-326): a VMS
	 * resource name may contain NULs, so the question must state how many
	 * bytes the name is. */
	int (*name_mastered_here)(void *ctx, const char *resnam,
				  uint32_t resnam_len, uint16_t group,
				  uint8_t mode);
	/*
	 * The directory hash this executive LEARNED for the name, if any
	 * (vms_dlm_proxy.h: a value some system in this cluster put on the
	 * wire, never computed here). 0 = none held, and then the 0x14 carries
	 * zeros there.
	 */
	int (*dir_hash)(void *ctx, const char *resnam, uint32_t resnam_len,
			uint16_t group, uint8_t mode, uint32_t *out_hash);

	/*
	 * WHICH MEMBER IS THIS NAME'S DIRECTORY NODE? (rd vms-083.) The second
	 * of the two systems that owe this tree an end-of-records, and the one
	 * this node must not wait for when it is the old master itself.
	 *
	 * Behind this door are the SAME TWO READS the requester path routes a
	 * directory lookup with, and no third: the hash the ENGINE holds for
	 * the name (wire-learned or determined for routing -- never computed
	 * for the asking, Rule 8) resolved through the connection manager's own
	 * weight vector (Davis p. 6-31/6-32). Returns 1 and writes `*out_csid`
	 * -- 0 meaning THIS node is the directory -- only when both reads
	 * succeeded; 0 means this node cannot name it, and then the directory
	 * is simply not waited for.
	 */
	int (*dir_node)(void *ctx, const char *resnam, uint32_t resnam_len,
			uint16_t group, uint8_t mode, uint32_t *out_csid);

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

	/*
	 * rd vms-26e: THE OLD MASTER'S VALUE BLOCK for one lock this node holds
	 * in a tree it is adopting (the op-0x1a; vms_dlm_adopt.h
	 * vms_lock_dlm_adopt_valblk). 1 when the ENGINE found that lock, the
	 * open adoption and the sender all as the frame says, 0 otherwise.
	 */
	int (*adopt_valblk)(void *ctx, const struct vms_dlm_adopt_valblk *v);

	/* The one-line-per-fact console voice. */
	void (*log)(void *ctx, const char *msg);

	/*
	 * THE HOLDER'S DOORS (rd vms-33a) -- vms_dlm_adopt.h's hand-over entry
	 * points, one each. OPTIONAL as a group: a table bound without them
	 * answers every step exactly as it did before and only COUNTS an
	 * op-0x11 (`ho_unbound`), which is the honest floor for a node that
	 * cannot reach its own proxies. All three, or the holder half is off.
	 *
	 * `ho_next` 1 = the engine named the next lock of ours to describe;
	 * `ho_rehome` / `ho_valblk` 1 = the engine really did it (SS$_NORMAL),
	 * the same 1/0 contract as the adoption doors above.
	 */
	int (*ho_next)(void *ctx, const struct vms_dlm_adopt_tree *t,
		       uint32_t new_master_csid,
		       struct vms_dlm_handover_rec *out);
	int (*ho_rehome)(void *ctx, uint32_t holder_lkid,
			 uint32_t old_master_csid, uint32_t new_master_csid,
			 uint32_t new_master_lkid);
	int (*ho_valblk)(void *ctx, const struct vms_dlm_adopt_valblk *v);

	/*
	 * THE FAILOVER REBUILD'S DOORS (rd vms-874f) -- OPTIONAL as a group, on
	 * top of the holder's three (which the rebuild reuses to describe and
	 * re-home our proxies) and the adoption doors (which it reuses when
	 * this node becomes a tree's master). Unbound, a departure is only
	 * recorded and nothing is sent: the honest floor.
	 *
	 * `rb_ready`       1 when the directory may be resolved: the weight
	 *                  vector is built, and either no transition is
	 *                  running or this table's rebuild window is open
	 *                  (vms_dlm_rm_rb_window_open, rd vms-2fa).
	 * `rb_tree_next`   vms_lock_dlm_rebuild_tree_next: the next orphaned
	 *                  tree of ours not among `skip`.
	 * `rb_dir_entry`   THIS node's own directory, asked about a root it
	 *                  directs: 0 no entry, 1 another system masters it
	 *                  (`*master`), 2 this node does.
	 * `rb_dir_claim`   record THIS node as that root's master in its own
	 *                  directory (vms_dlm_dir_claim_self); 1 when held.
	 * `rb_install`     vms_lock_dlm_rebuild_lock: a survivor's lock, as
	 *                  the tree's master; 1 = SS$_NORMAL.
	 */
	int (*rb_ready)(void *ctx);
	int (*rb_tree_next)(void *ctx, uint32_t departed_csid,
			    const struct vms_dlm_adopt_tree *skip,
			    uint32_t n_skip, struct vms_dlm_adopt_tree *out);
	int (*rb_dir_entry)(void *ctx, const struct vms_dlm_res_ident *id,
			    uint32_t *master);
	int (*rb_dir_claim)(void *ctx, const struct vms_dlm_res_ident *id);
	int (*rb_install)(void *ctx, const struct vms_dlm_adopt_lock_rec *r,
			  struct vms_dlm_adopt_lock_result *out);
	/*
	 * rd vms-1b68, both OPTIONAL. `rb_unsent`: a rebuild of `t` ENDED
	 * (done, stopped, or -- ident_valid 0 -- never startable for
	 * t->old_master_csid): end every waiting survivor still naming the
	 * departed master SS$_UNSUPPORTED (vms_lock_dlm_rebuild_fail_unsent,
	 * plus the requester FSM's block). Returns how many. `rb_settle`: the
	 * window closed -- decide the waiters this node masters once
	 * (vms_lock_dlm_rebuild_settle).
	 */
	uint32_t (*rb_unsent)(void *ctx, const struct vms_dlm_adopt_tree *t);
	void (*rb_settle)(void *ctx);
};

/* ==========================================================================
 * THE HOLDER'S HALF (rd vms-33a): a real VMS node moves a tree THIS node holds
 * locks in -- or is the directory node of -- to a THIRD node, and that new
 * master waits for this node's records and its op-0x13 (rule W2). The
 * measured shape is vms_cluster_codec_dlm.h H1..H4; this is the machine.
 *
 *   op 0x0f from O   ARM: the frame NAMES the new master N (H1). N is
 *                    addressed only if the club says it is a SELECTED member
 *                    other than this node and other than O; otherwise the
 *                    hand-over is not armed (`ho_unaddressable`).
 *   op 0x11 from O   FIRE: everyone told an 0x11 owes N an 0x13 (H2). Ask
 *                    the engine for the next lock of ours in the tree that
 *                    still names O; send its record; wait for the answer.
 *   82/01 from N     REPLACE that proxy's master and handle with N's; then
 *                    the next record (a sub-lock becomes ready only now --
 *                    its record carries the handle N just minted for its
 *                    parent) -- or, when none is left, the op-0x13.
 *   op 0x1a from O   the resource's value block, written onto OUR resource
 *                    so the record relays it (rd vms-26e), admitted only from
 *                    an O that has armed a hand-over here.
 *
 * ONE RECORD IN FLIGHT PER TREE (H3): the commonest real pacing, and the only
 * one a sub-record allows. INV-6: a slot holds NO lock id it puts on the wire
 * except `await_lkid`, which is the engine's answer to "which lock did I just
 * describe" and is used only to correlate N's 82/01; every record field is
 * re-read from the engine at the moment its frame is built.
 *
 * WHAT IT REFUSES TO DO. An 82/01 that is not a grant (outcome != 0xfa, or no
 * handle) STOPS the hand-over -- counted and said -- rather than send an
 * op-0x13 that tells N this node holds less than it does. A stalled new master
 * is a hang; a new master told the wrong lock set is a stale handle on a real
 * VAX's release path (rd vms-3ac's INVLOCKID).
 * ========================================================================== */
#define VMS_DLM_HO_TREES  16u

enum vms_dlm_ho_state {
	VMS_DLM_HO_ST_FREE = 0,
	VMS_DLM_HO_ST_ARMED,      /* the 0x0f named N; the 0x11 has not come */
	VMS_DLM_HO_ST_AWAIT,      /* a record is out; its 82/01 is not in     */
	VMS_DLM_HO_ST_ENDED,      /* the 0x13 is out; N's 0x14 is not in yet  */
	VMS_DLM_HO_ST__COUNT
};

struct vms_dlm_ho_tree {
	struct vms_dlm_adopt_tree tree;   /* root identity + the OLD master  */
	struct vms_dlm_res_ident  id;     /* the same, as the wire spells it */
	uint32_t new_master;              /* N, as the 0x0f named it, checked */
	uint32_t await_lkid;              /* the lock whose 82/01 is due      */
	uint32_t records;                 /* records N answered for this tree */
	uint8_t  state;                   /* enum vms_dlm_ho_state            */
	uint8_t  pad[3];
};

/* ==========================================================================
 * THE FAILOVER REBUILD (rd vms-874f): a tree's MASTER LEFT the cluster, and
 * this node -- a surviving holder -- re-describes every lock it holds in that
 * tree, one op-0x08 record at a time (vms_cluster_codec_dlm.h R1..R4; capture
 * and census tests/lab/captures/vms-874f-failover-rebuild-20261011). VMS does
 * NOT remaster a user tree at shutdown; without these records a survivor's
 * locks die with the master.
 *
 *   a departure      recorded (`rb_departed`); the rebuild waits for the
 *                    directory (`rb_ready`: vector built, no transition).
 *   KICK             for each departed master D, the engine names the next
 *                    ORPHANED tree of ours (a granted proxy still naming D).
 *                    Its directory node X is resolved the way every lookup
 *                    is (the `dir_node` door):
 *                    A record that would go to ANOTHER system goes only
 *                    while the REBUILD WINDOW is open (rd vms-bcb,
 *                    vms-2fa; below); outside it the tree is STOPPED,
 *                    counted.
 *                      X another member  -> the ROOT record goes to X (R2);
 *                      X is THIS node    -> its own directory is asked: an
 *                        entry naming another member M (a survivor it has
 *                        just answered 0xf9) -> the ROOT goes to M (RA1b's
 *                        directory-holder case); no entry -> this node
 *                        becomes the master itself, claims the entry, and
 *                        sends nothing.
 *   82/01 0xf9       THIS node now masters the tree (R3): adopted through
 *                    the adoption engine, whose close turns every proxy of
 *                    ours in it into a local lock -- and NO sub-record goes
 *                    out (a 0xf9 holder sends none, RA1b's VAX2 RMT1).
 *   82/01 0xfa       re-home that proxy onto the answerer and the handle it
 *                    minted (vms_lock_dlm_handover_rehome), then the next
 *                    record -- a sub becomes ready only now, carrying that
 *                    handle at body[40:44] (R4) -- or, none left, the tree
 *                    is done. There is no end-of-records message (the census
 *                    finds none).
 *   anything else    the tree STOPS, counted and said: never a claim to hold
 *                    less than this node holds.
 *
 * AND AS THE TREE'S NEW MASTER, inbound: a survivor's op-0x08 ROOT for a tree
 * this node came to master by a rebuild, or a SUB under a lock this node
 * already holds for that survivor, is installed by the engine
 * (vms_lock_dlm_rebuild_lock) and answered 0xfa off the LKB it created. A ROOT
 * for any other tree is NOT_MINE: the directory role answers it as before.
 *
 * INV-6 as in the holder's half: a slot holds an identity, a destination and
 * the lock id whose answer is due; every record cell is re-read from the
 * engine when its frame is built.
 * ========================================================================== */
/*
 * THE REBUILD WINDOW (rd vms-2fa; tests/lab/captures/
 * vms-2fa-rebuild-step-20261102). Every real op-0x08 -- 1,965 of 1,965 whose
 * barrier is in a capture -- goes out INSIDE the departure's transition: after
 * the barrier release (cat-0x01 op-0x0c, step at body[16]) of
 *
 *   R = VMS_DLM_RB_REL_WEIGHTED (5)   when the SENDER's own LOCKDIRWT != 0,
 *   R = VMS_DLM_RB_REL_UNWEIGHTED (6) when it is 0,
 *
 * and every record is answered before the sender's own next barrier frame (its
 * step report R+1 as a participant, its release R+1 as the coordinator). Not
 * the transition type, not a directory-vector change, not the coordinator's
 * weight: the census separates each of those from the rule. The one OVMX record
 * sent outside it (after release 12, lab33a) bugchecked a real VAX LOCKMGRERR.
 *
 * So the window is the barrier's own state, told to this table by the barrier
 * FSMs through the arm (vms_dlm_rm_rb_barrier_released): it OPENS on release
 * R, and CLOSES on any other release, at a transition's begin and end, and
 * never otherwise. While open, `vms_dlm_rm_rb_outstanding` is what the barrier
 * holds its next frame on.
 *
 * AN ANSWER THAT NEVER COMES (vms_dlm_rm_rb_expire). Every real record in the
 * corpus was answered, and a destination that LEAVES already stops its tree
 * (rb_dst_departed). A destination that stays connected and stays silent is
 * bounded by the interval the cluster already allows a silent system,
 * RECNXINTERVAL: past it the tree is STOPPED, counted (rb_answer_expired) and
 * said, and the barrier goes on. The same interval after which VMS would have
 * removed that system, and with it the hold -- never a timeout of this
 * table's own choosing.
 */
#define VMS_DLM_RB_REL_WEIGHTED   5u
#define VMS_DLM_RB_REL_UNWEIGHTED 6u

#define VMS_DLM_RB_TREES     16u
#define VMS_DLM_RB_DEPARTED   8u

enum vms_dlm_rb_state {
	VMS_DLM_RB_ST_FREE = 0,
	VMS_DLM_RB_ST_AWAIT,      /* a record is out; its 82/01 is not in      */
	VMS_DLM_RB_ST_STOPPED,    /* could not complete: left, never retried   */
	VMS_DLM_RB_ST__COUNT
};

struct vms_dlm_rb_tree {
	struct vms_dlm_adopt_tree tree;   /* root identity + the DEPARTED master */
	struct vms_dlm_res_ident  id;     /* the same, as the wire spells it    */
	uint32_t dst;                     /* where the records go (R2)          */
	uint32_t await_lkid;              /* the lock whose 82/01 is due        */
	uint32_t records;                 /* records answered 0xfa so far       */
	uint8_t  state;                   /* enum vms_dlm_rb_state              */
	uint8_t  await_root;              /* the outstanding record is a ROOT   */
	uint8_t  pad[2];
};

/* A tree THIS node came to master by a rebuild: the authority under which a
 * survivor's ROOT record is installed here. */
struct vms_dlm_rb_mastered {
	struct vms_dlm_res_ident id;
	uint32_t departed;                /* whose tree it was                  */
	uint8_t  used;
	uint8_t  pad[3];
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
	uint32_t ends_unexpected;    /* 0x13 from a system that is no member   */
	uint32_t ends_admitted;      /* 0x13 from a MEMBER the owed set did    */
				     /* not predict: admitted and acked        */
	uint32_t owed_dir_named;     /* trees whose directory node this node   */
				     /* could name and therefore waits for     */
	uint32_t owed_dir_unknown;   /* ... and the ones it could not: the     */
				     /* directory is not waited for            */
	uint32_t owed_overflow;      /* a member that would not fit the set    */
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
	uint32_t valblks_taken;      /* op-0x1a the engine accepted (rd vms-26e)*/
	uint32_t valblks_refused;    /* ... and the ones it did not: no lock,  */
				     /* no open adoption, or not the sender's  */

	/* THE HOLDER'S HALF (rd vms-33a). */
	struct vms_dlm_ho_tree ho[VMS_DLM_HO_TREES];
	uint32_t ho_armed;           /* hand-overs a 0x0f armed               */
	uint32_t ho_unaddressable;   /* 0x0f whose named new master is not a  */
				     /* member this node may address          */
	uint32_t ho_no_slot;         /* ...armable, but every slot was busy   */
	uint32_t ho_unarmed;         /* 0x11 for a tree no 0x0f armed: N is   */
				     /* unknown, so NOTHING is sent           */
	uint32_t ho_unbound;         /* 0x11 with the holder doors unbound    */
	uint32_t ho_repeats;         /* 0x11 for a hand-over already sending  */
	uint32_t ho_records_sent;    /* op-0x12 records really on the wire    */
	uint32_t ho_records_unsent;  /* ...owed and not sent (stops the tree) */
	uint32_t ho_answers;         /* 82/01s that re-homed a proxy of ours  */
	uint32_t ho_answers_refused; /* 82/01s not a grant, or the engine     */
				     /* would not re-home: the tree STOPS     */
	uint32_t ho_ends_sent;       /* op-0x13s really on the wire           */
	uint32_t ho_ends_unsent;     /* ...owed and not sent                  */
	uint32_t ho_held_acked;      /* N's op-0x14 to us, answered 82/15     */
	uint32_t ho_held_unacked;    /* ...an 82/15 owed and not staged       */
	uint32_t ho_departed;        /* hand-overs dropped: N left            */
	uint32_t ho_valblks_taken;   /* op-0x1a blocks taken as a holder      */

	/* THE FAILOVER REBUILD (rd vms-874f). */
	uint32_t rb_departed[VMS_DLM_RB_DEPARTED];  /* masters still rebuilding */
	struct vms_dlm_rb_tree     rb[VMS_DLM_RB_TREES];
	struct vms_dlm_rb_mastered rbm[VMS_DLM_RB_TREES];
	uint32_t rb_departures;      /* departures recorded for a rebuild     */
	uint32_t rb_departed_full;   /* ...not recorded: the list was full    */
	uint32_t rb_unbound;         /* kicks with the rebuild doors unbound  */
	uint32_t rb_trees_started;   /* orphaned trees of ours taken up       */
	uint32_t rb_records_sent;    /* op-0x08 records really on the wire    */
	uint32_t rb_records_unsent;  /* ...owed and not sent (tree STOPPED)   */
	uint32_t rb_answers;         /* 0xfa answers that re-homed a proxy    */
	uint32_t rb_answers_refused; /* non-grant / un-re-homable answers     */
	uint32_t rb_trees_done;      /* trees whose every lock was re-homed   */
	uint32_t rb_mastered;        /* trees this node became master of      */
	uint32_t rb_master_refused;  /* ...the adoption engine refused        */
	uint32_t rb_claim_failed;    /* mastered, and this node's own         */
				     /* directory could not record it         */
	uint32_t rb_unaddressable;   /* no directory/master this node may     */
				     /* address: STOPPED, nothing sent        */
	uint32_t rb_dst_departed;    /* trees stopped: their destination left */
	uint32_t rb_out_of_window;   /* trees stopped: a record would have    */
				     /* gone out outside the rebuild step     */
	uint32_t rb_answer_expired;  /* trees stopped: no answer within the   */
				     /* cluster's RECNXINTERVAL               */
	uint32_t rb_windows_opened;  /* rebuild windows opened (release R)    */
	uint32_t rb_window_opened_ms;/* when the open window opened           */
	uint8_t  rb_window_open;     /* 1 between release R and the next      */
	uint8_t  rb_window_step;     /* the R that opened it                  */
	uint8_t  rb_window_pad[2];
	uint32_t rb_no_slot;         /* trees left waiting: every slot busy   */
	uint32_t rb_waiters_ended;   /* survivors no record carried, ended    */
				     /* SS$_UNSUPPORTED (rd vms-1b68)         */
	uint32_t rb_in_installed;    /* survivors' records installed + 0xfa   */
	uint32_t rb_in_refused;      /* ...refused by the engine: unanswered  */
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
 * VMS_DLM_RM_NOT_MINE for anything that is not one of the five steps, the
 * op-0x11 or the op-0x1a, so the caller's own routing is unchanged by this
 * file's presence.
 *
 * THE op-0x11 (rd vms-33a) is the holder's half: HANDLED whenever its sender
 * can be named and it parses, whether or not this node then sends anything
 * (counted either way); nothing is ever staged for it.
 *
 * THE op-0x1a (rd vms-26e) is not a table event: it names no tree, only a lock
 * of ours, so it goes straight to the engine (`adopt_valblk`), which finds the
 * tree from the lock. It is a NOTIFICATION -- 1205 of 1205 real ones are never
 * answered -- so nothing is staged either way: HANDLED when the engine took
 * it, REFUSED (counted) when it did not.
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

/*
 * op-0x11: some OTHER member is the new master of a tree. A real VMS member
 * never answers one (7 of 7 carry no transaction). vms_dlm_rm_step now ACTS on
 * it (the holder's half above) and counts it; this is the count alone, for an
 * arm whose step was refused before the table could see the frame.
 */
void vms_dlm_rm_notified(struct vms_dlm_rm *f);

/*
 * AN INBOUND cat-0x82 op-0x01 that may be the new master's answer to one of
 * this node's op-0x12 records. 1 when it was (consumed, and the hand-over
 * moved on or stopped -- both counted); 0 otherwise, and then the caller's own
 * reply path sees the frame unchanged. Matched by the sender being the slot's
 * new master AND body[24:28] being the lock whose record is out: the only two
 * facts that tie an answer to a record (the connection manager's transaction
 * token is not surfaced to the DLM).
 */
int vms_dlm_rm_ho_answer(struct vms_dlm_rm *f, uint32_t from_csid,
			 uint8_t category, uint8_t opcode,
			 const uint8_t *body, uint32_t len);

/* How many holder slots are in `st`. */
uint32_t vms_dlm_rm_ho_in_state(const struct vms_dlm_rm *f,
				enum vms_dlm_ho_state st);

/*
 * A member LEFT. It is struck from every tree's owed set -- a departed system
 * will not be sending an end-of-records -- which may complete a tree that was
 * waiting only for it. And (rd vms-874f) it is RECORDED as a master whose
 * trees this node may hold locks in, and the rebuild is kicked.
 */
void vms_dlm_rm_member_departed(struct vms_dlm_rm *f, uint32_t csid);

/*
 * THE FAILOVER REBUILD (rd vms-874f), its three doors from the arm.
 *
 * `vms_dlm_rm_rb_kick` takes up every orphaned tree it has a slot for --
 * idempotent, and a no-op while `rb_ready` says the directory cannot be
 * resolved, so the arm calls it at a departure, at a transition's end and on
 * its beat.
 *
 * `vms_dlm_rm_rb_answer` is an inbound cat-0x82 op-0x01: 1 when it was the
 * answer to one of our op-0x08 records (matched by sender == the record's
 * destination AND body[24:28] == the lock whose record is out), 0 otherwise.
 *
 * `vms_dlm_rm_rb_record` is an inbound cat-0x02 op-0x08 from `from_csid`:
 * HANDLED with the 0xfa answer staged; REFUSED (a SUB the engine would not
 * install, or a sender that is not a member -- never answered, counted);
 * NOT_MINE for a ROOT of a tree this node did not come to master by a rebuild,
 * which the directory role then answers exactly as before.
 *
 * `vms_dlm_rm_rb_transition_begin`: a new transition -- the trees this node
 * mastered by an EARLIER rebuild stop being admissible for ROOT records.
 */
void vms_dlm_rm_rb_kick(struct vms_dlm_rm *f);
int vms_dlm_rm_rb_answer(struct vms_dlm_rm *f, uint32_t from_csid,
			 uint8_t category, uint8_t opcode,
			 const uint8_t *body, uint32_t len);
enum vms_dlm_rm_status vms_dlm_rm_rb_record(struct vms_dlm_rm *f,
					    uint32_t from_csid,
					    const uint8_t *body, uint32_t len);
void vms_dlm_rm_rb_transition_begin(struct vms_dlm_rm *f);

/*
 * THE REBUILD WINDOW'S DOORS (rd vms-2fa; the window section above).
 *
 * `vms_dlm_rb_release_step`: R for a node whose own LOCKDIRWT is
 * `own_lockdirwt` -- 5 when non-zero, else 6.
 *
 * `vms_dlm_rm_rb_barrier_released`: the barrier released step `step` (a
 * participant RECEIVED it; the coordinator SENT it to everybody). Release R
 * opens the window (stamped `now_ms`) and kicks the rebuild, so this node's
 * records go out NOW; any other release closes it.
 *
 * `vms_dlm_rm_rb_window_close`: a transition began or ended.
 *
 * `vms_dlm_rm_rb_outstanding`: records on the wire and not yet answered -- what
 * the barrier holds its next frame on.
 *
 * `vms_dlm_rm_rb_expire`: with the window open longer than `limit_ms`, every
 * tree still awaiting an answer is STOPPED, counted (rb_answer_expired);
 * returns how many. A `limit_ms` of 0 expires nothing.
 */
uint32_t vms_dlm_rb_release_step(uint32_t own_lockdirwt);
void vms_dlm_rm_rb_barrier_released(struct vms_dlm_rm *f, uint32_t step,
				    uint32_t own_lockdirwt, uint32_t now_ms);
void vms_dlm_rm_rb_window_close(struct vms_dlm_rm *f);
int vms_dlm_rm_rb_window_open(const struct vms_dlm_rm *f);
uint32_t vms_dlm_rm_rb_outstanding(const struct vms_dlm_rm *f);
uint32_t vms_dlm_rm_rb_expire(struct vms_dlm_rm *f, uint32_t now_ms,
			      uint32_t limit_ms);

/* How many rebuild slots are in `st`; and the rebuild's stopped total. */
uint32_t vms_dlm_rm_rb_in_state(const struct vms_dlm_rm *f,
				enum vms_dlm_rb_state st);
uint32_t vms_dlm_rm_rb_stopped(const struct vms_dlm_rm *f);

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
