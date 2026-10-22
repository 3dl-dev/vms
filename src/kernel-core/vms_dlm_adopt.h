/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_dlm_adopt.h - THE REMASTER ADOPTION SEAM (rd vms-14fd, under vms-50d).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.4 (RSB/LKB/directory),
 * SS3.6 D-DLM-4/D-DLM-5. Measured wire shape: tests/lab/captures/
 * vms-380-remaster-arms-20261011/ (arms RA2/RA3/RA4) and
 * tests/lab/captures/vms-50d-ra5-14cells-20261013/ (arm RA5).
 *
 * ===========================================================================
 * WHAT A REMASTER ADOPTION IS, AS REAL V7.3 VAXES DO IT
 * ===========================================================================
 * When a lock tree moves from its old master O to a new master N, the measured
 * exchange (RA2/RA4/RA5, three VAXes, SDA truth before and after) is:
 *
 *   O -> N    op 0x0e, op 0x0f, op 0x10          (each answered 82/15)
 *   O -> H    op 0x0f, op 0x11                   (every other member)
 *   H -> N    op 0x12, one record PER LOCK the holder H holds in the tree,
 *             ROOT FIRST and then its sub-resources. N answers each 82/01
 *             with THE HANDLE N ITSELF MINTED for that lock
 *             (RA2: 010003BA..BD == the master copies SDA showed on VAX3).
 *   H -> N    op 0x13, the holder's end-of-records marker
 *   O -> N    op 0x13, the old master's end-of-records marker
 *   N -> O    op 0x14, and N -> H op 0x14: the tree is held
 *
 * This header is the engine door for the MIDDLE of that: the 0x12 records and
 * their 0x13. It is deliberately NOT a codec and NOT a sender -- the arm parses
 * the frames (rd vms-380) and the arm sends; the engine's job is to end up
 * HOLDING THE TREE FOR REAL, with a real RSB per resource and a real LKB per
 * remote holder's lock, so that:
 *
 *   - the 82/01 answer's body[20:24] is a lock id THIS engine minted, read back
 *     off the LKB it names (never a counter, never an echo), and
 *   - the holder's NEXT op-0x01 / op-0x07 / op-0x03 on any of those locks is
 *     served by the shipping master path (vms_dlm_master.h), because the locks
 *     it names genuinely exist here.
 *
 * ===========================================================================
 * WHAT THIS SEAM STILL DOES NOT DO: BUILD OR SEND THE 0x14
 * ===========================================================================
 * No function here builds or sends an op-0x14 -- that is the wire arm's, and
 * the codec's, through vms_dlm_rm_fsm.h. What this seam owes it is the TRUTH
 * the frame asserts: `vms_lock_dlm_adopt_end` returns SS$_NORMAL only when an
 * adoption of exactly that root resource really was open here and its locks
 * really are installed, and `vms_lock_dlm_name_mastered_here` is a second,
 * independent read of the same claim. A 0x14 sent without both is a node
 * telling a real VAX it holds a tree it does not have.
 *
 * `vms_lock_dlm_adopt_unreported()` still counts the completions nobody was
 * told about (`completed` minus `reports`), so the gap stays a number this
 * executive reports rather than a silence. When vms-380 settled the frame it
 * settled it in the direction that asserts LESS: of the 132-byte body the
 * op-0x14 names six fields and zeroes the rest, and every one of the four
 * cells RA5/RA6 thought meaningful turned out, at n=68, to be the sender's
 * stale buffer. Two real lab peers have already been crashed by minted fields;
 * rd vms-6ad1 (RA7) is what proves a real VAX does not read that span.
 *
 * ===========================================================================
 * INV-6 IN THE SHAPE OF THE STRUCTS
 * ===========================================================================
 * Every field below is READ OFF A RECEIVED FRAME by the arm, or off the
 * connection manager's own identification of the sender. There is no field a
 * caller may use to tell the engine a value the engine then asserts back: the
 * RESULT struct is filled by reading the LKB and RSB the engine just created.
 * An adoption that cannot install a real lock installs none and says so.
 *
 * INCLUDES: kernel-core headers only (CI gate
 * tools/ci/cluster_core_includes_gate.sh RULE 1/2).
 */
#ifndef OVMX_VMS_DLM_ADOPT_H
#define OVMX_VMS_DLM_ADOPT_H

#include "vms_wire_types.h"   /* fixed-width types, substrate-selected once */
#include "vms_dlm_proxy.h"    /* VMS_DLM_VALBLK_LEN, VMS_DLM_LKID_UNSET */

/*
 * THE TREE BEING HANDED OVER.
 *
 * Its ROOT resource's full identity (rd vms-b5b0: a VMS resource name is
 * qualified by the UIC group and the access mode), plus the system handing it
 * over. `ident_valid` 0 is REFUSED, never defaulted: "which resource of that
 * name" has no honest default, and guessing it adopts somebody else's tree.
 */
struct vms_dlm_adopt_tree {
	char     resnam[32];        /* the ROOT resource name, body[48..]   */
	uint32_t old_master_csid;   /* the system handing the tree over      */
	uint16_t res_group;         /* body[44:46]                           */
	uint8_t  res_mode;          /* body[46]                              */
	uint8_t  ident_valid;       /* 1 when group/mode came off the frame  */
};

/*
 * ONE HOLDER'S LOCK, as its op-0x12 record named it.
 *
 * Cell provenance, all SDA-named in RA2/RA4/RA5:
 *   holder_csid         the CONNECTION MANAGER's identification of the sender,
 *                       never a body field (spec SS4(a)).
 *   holder_lkid         body[24:28] -- the holder's OWN lock id (4/4 vs SDA).
 *   parent_master_lkid  body[40:44] -- the parent's NEW-MASTER lock id, i.e. a
 *                       handle THIS engine minted and answered in the 82/01 for
 *                       the parent record. 0 on the tree's ROOT record. It is
 *                       LOOKED UP here, never trusted: a record naming a handle
 *                       this engine did not mint for this adoption is refused.
 *   granted_mode        body[30], requested_mode body[31] (CR 01 01, CW 02 02
 *                       vs SDA 4/4 in RA2).
 *   valblk/_seq/_present
 *                       the record's own copy of the resource's value block
 *                       and its SEQUENCE -- body[88:104] and body[104:108]
 *                       (rd vms-ed5; 420 of 420 real 82/01 answers return
 *                       exactly that span). ABSENT means the resource's own
 *                       block is LEFT ALONE: twenty zeros presented as an LVB
 *                       is the placeholder INV-6 forbids.
 */
struct vms_dlm_adopt_lock_rec {
	char     resnam[32];           /* this lock's resource name          */
	uint32_t holder_csid;
	uint32_t holder_lkid;
	uint32_t parent_master_lkid;   /* 0 => the tree's root record        */
	uint16_t res_group;
	uint8_t  res_mode;
	uint8_t  ident_valid;
	uint8_t  granted_mode;         /* LCK$K_ encoding                    */
	uint8_t  requested_mode;       /* LCK$K_ encoding                    */
	uint8_t  valblk_present;
	uint8_t  pad;
	uint8_t  valblk[VMS_DLM_VALBLK_LEN];
	uint32_t valblk_seq;           /* body[104:108]                      */
};

/*
 * WHAT THE ENGINE DID WITH ONE RECORD -- every field READ BACK off the objects
 * it created, which is what makes the 82/01 answer an executive read.
 */
struct vms_dlm_adopt_lock_result {
	uint32_t master_lkid;    /* 82/01 body[20:24]: the handle WE minted  */
	uint32_t holder_lkid;    /* 82/01 body[24:28]: read back off the LKB */
	uint8_t  granted_mode;   /* read back off the LKB                    */
	uint8_t  valblk_present; /* 1 when the RSB holds a block or a sequence */
	uint8_t  duplicate;      /* 1 = a RETRANSMITTED record; same handle   */
	uint8_t  pad;
	uint8_t  valblk[VMS_DLM_VALBLK_LEN];   /* the RSB's block, read back */
	uint32_t valblk_seq;                   /* ...and its sequence        */
};

/* ==========================================================================
 * THE TRANSACTION
 * ========================================================================== */

/*
 * OPEN an adoption of `t`. This node records itself as the tree's master and
 * opens the transaction the 0x12 records belong to.
 *
 * REFUSED (and nothing changed) when: the identity is incomplete; this node has
 * no cluster identity of its own (vms_local_csid unassigned -- a node that does
 * not know who it is may not assert mastery); no delivery proc is registered
 * (vms_dlm_master.h condition 4); or an adoption of that tree is already open.
 *
 * Returns SS$_NORMAL on a real open, an SS$_ error otherwise.
 */
uint32_t vms_lock_dlm_adopt_open(const struct vms_dlm_adopt_tree *t);

/*
 * ADOPT ONE LOCK from a 0x12 record into the open adoption of `t`.
 *
 * Creates a REAL master-side LKB on a REAL resource block: granted at the mode
 * the holder really held, tagged with the holder's CSID and its own handle
 * (vms_dlm_master.h condition 2), owned by the delivery proc at PSL_C_KERNEL
 * (condition 1), and -- for a sub-resource -- linked under the parent LOCK this
 * engine minted, on a resource block created under the parent RESOURCE. The
 * tree is therefore a tree here, not a flat set of same-named roots.
 *
 * IDEMPOTENT to a retransmit, like the master's ENQ path (D-DLM-5): the same
 * (holder_csid, holder_lkid) in the same adoption gets the SAME handle back and
 * `duplicate` 1. Minting a second LKB would double the tree.
 *
 * Returns SS$_NORMAL only when `*out` describes a lock that now exists.
 */
uint32_t vms_lock_dlm_adopt_lock(const struct vms_dlm_adopt_tree *t,
				 const struct vms_dlm_adopt_lock_rec *r,
				 struct vms_dlm_adopt_lock_result *out);

/*
 * CLOSE the adoption of `t` (the 0x13 end-of-records). `*n_locks` (optional) is
 * how many locks the engine REALLY installed -- a counted observation, and the
 * number any later statement about this tree has to be consistent with.
 *
 * Completing an adoption sends NOTHING. See this header's 0x14 note.
 *
 * AND IT CONVERTS THIS NODE'S OWN LOCKS IN THE TREE OUT OF PROXY STATE
 * (rd vms-3ac). A tree this node has been handed is a tree this node masters,
 * so a lock of its own on it is no longer a requester-side image of somebody
 * else's lock: a granted one becomes a master-side granted lock at the mode it
 * is already held at, a pending one is decided by this node's own queues, and
 * neither keeps the old master's handle. That is what makes the release of such
 * a lock LOCAL by construction rather than a frame naming a handle the old
 * master cannot resolve -- the frame that bugchecked a real V7.3 node with
 * INVLOCKID (tests/lab/captures/vms-3ac-invlockid-20261021).
 *
 * An ABORTED adoption promotes nothing: it gives the tree back.
 */
uint32_t vms_lock_dlm_adopt_end(const struct vms_dlm_adopt_tree *t,
				uint32_t *n_locks);

/*
 * ABANDON the adoption of `t`: RELEASE every lock it installed, through the
 * shipping master-side release path, and give the tree's mastery back to the
 * old master. The arm calls this when a record is refused mid-tree -- a HALF
 * adopted tree must never be served, because the locks it is missing are locks
 * a holder still believes it holds.
 *
 * `*n_undone` (optional) is how many locks really went.
 */
uint32_t vms_lock_dlm_adopt_abort(const struct vms_dlm_adopt_tree *t,
				  uint32_t *n_undone);

/* ==========================================================================
 * READBACKS -- what the executive actually holds, for the arm and the tests
 * ========================================================================== */

/* Is an adoption of this tree OPEN right now? */
int vms_lock_dlm_adopt_is_open(const struct vms_dlm_adopt_tree *t);

/*
 * WHICH TREE did this engine mint `master_lkid` inside? (rd vms-ed5)
 *
 * A 0x12 record for a SUB-resource names its OWN resource, never the tree's
 * root, so the only thing that places it in a tree is the PARENT handle at
 * body[40:44] -- a handle this engine minted and answered in the 82/01 for the
 * parent record moments earlier. The wire arm therefore has to ASK, and it asks
 * the engine rather than keeping a table of its own: the lock database is where
 * the truth is, and a handle no OPEN adoption minted must not place a record in
 * any tree.
 *
 * Fills `*out` -- the root resource's identity, read off the adoption's own
 * root RSB, plus the old master this adoption was opened for -- and returns 1,
 * ONLY when `master_lkid` names a non-proxy lock installed by an adoption that
 * is open right now. Returns 0 and leaves `*out` zeroed otherwise, which is
 * what a forged or stale parent handle gets.
 */
int vms_lock_dlm_adopt_tree_of_lkid(uint32_t master_lkid,
				    struct vms_dlm_adopt_tree *out);

/*
 * THIS NODE REALLY TOLD THE CLUSTER about one completed adoption (rd vms-ed5):
 * at least one of the op-0x14s it owed went out. Raises `_adopt_reports()` and
 * so lowers `_adopt_unreported()`, which is the number of completions NOBODY
 * was told about. Called by the wire arm AFTER the sends -- never before, or
 * the counter would say a tree was announced that nobody was told about. A
 * member that was owed one and did not get it is counted per frame by the arm
 * (vms_dlm_rm_fsm.h `held_unsent`); the two answer different questions.
 */
void vms_lock_dlm_adopt_reported(void);
uint32_t vms_lock_dlm_adopt_reports(void);

/* How many locks has the OPEN adoption of this tree installed so far? -1 when
 * no adoption of it is open. */
int vms_lock_dlm_adopt_installed(const struct vms_dlm_adopt_tree *t);

/*
 * THIS NODE'S OWN LOCKS THAT ADOPTIONS CONVERTED out of proxy state, since boot
 * (rd vms-3ac). The counted half of the sentence above, and the one number that
 * tells the fix from the fail-safe: when this rises,
 * vms_lock_dlm_proxy_self_mastered() -- the posts dlm_proxy_post refused
 * because this node masters the resource -- does not have to.
 */
uint32_t vms_lock_dlm_adopt_promotions(void);

/* Adoptions opened / completed / refused, since boot. Diagnostics, never a
 * substitute for reading the lock database. */
uint32_t vms_lock_dlm_adoptions_opened(void);
uint32_t vms_lock_dlm_adoptions_completed(void);
uint32_t vms_lock_dlm_adoptions_refused(void);

/*
 * COMPLETED ADOPTIONS THIS NODE COULD NOT REPORT, i.e. `completed` minus
 * `reports`. The number exists so the gap is REPORTED rather than silent --
 * the same discipline as vms_lock_dlm_dir_removes_unposted(). An executive
 * with no wire arm bound reports every completion as unreported, which is
 * exactly true of it.
 */
uint32_t vms_lock_dlm_adopt_unreported(void);

#endif /* OVMX_VMS_DLM_ADOPT_H */
