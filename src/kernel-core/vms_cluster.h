/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster.h - the per-node VMScluster context, and the identity vocabulary
 * every cluster layer shares (FC-P0.1).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.1 (layering), SS3.4 (data
 * model), SS3.5 (interfaces), SS3.9 rule 3 ("no globals except one per-node
 * struct vms_cluster passed explicitly").
 *
 * WHAT THIS HEADER IS. The cluster stack is five layers -- port (vms_pe),
 * SCS (vms_scs), connection manager (vms_cnxman), the lock manager's SYSAP arm
 * (vms_dlm_scs) and MSCP -- each with its own private state. They are NOT five
 * globals: they hang off ONE per-node context, and every entry point in the
 * stack takes a `struct vms_cluster *`. That is what makes the whole stack
 * instantiable N times inside one host process, which is what the rung-2 host
 * cluster simulator does (design SS3.9: 2..8 simulated nodes on a virtual LAN
 * with a virtual clock, in milliseconds, deterministic by seed). A global would
 * cost that simulator, and the simulator is the biggest single accelerator in
 * the plan.
 *
 * WHAT IT IS NOT. It is not a mirror of anything on the wire and it holds no
 * value that did not come from the executive or from SYSGEN. Every field below
 * names where it comes from; a field the executive has not learned yet carries
 * an explicit `*_valid` flag and is HONESTLY ABSENT until then (INV-6). The
 * predecessor of this file is the strawman it replaces: a
 * populated-by-ioctl vms_cluster_members[96] table that SHOW CLUSTER read, whose
 * local CSID was an insmod parameter defaulting to 1 -- a phantom cluster of one.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 *
 * THIS HEADER DELIBERATELY DOES NOT INCLUDE exec_kbackend.h. Every cluster layer
 * header includes this one, so a seam include here would drag the whole
 * substrate backend -- <linux/spinlock.h> or <sys/mutex.h> -- into the host unit
 * tests and the N-node simulator, which are rungs 1 and 2 of the test ladder and
 * must build with a plain host compiler and NO kernel headers. What this header
 * needs is fixed-width integers and nothing else, so that is all it asks for:
 *   kernel build  -> vms_internal.h, the substrate's own type + SS$_ vocabulary
 *   host build    -> <stdint.h>, a FREESTANDING C header that is neither Linux
 *                    nor NetBSD (select it with -DOVMX_CLUSTER_HOST)
 * The one seam type the per-node context would otherwise hold -- the fork mutex
 * -- lives inside the opaque struct vms_cluster_fork instead (FC-P0.5), which is
 * where design SS3.3 puts the serialization anyway.
 */
#ifndef OVMX_VMS_CLUSTER_H
#define OVMX_VMS_CLUSTER_H

#if defined(OVMX_CLUSTER_HOST)
#  include <stdint.h>
   /* NULL and size_t, for the pure layer TUs (FC-P3.6's CLUB/CSB model is the
    * first) that take pointers. <stddef.h> is a FREESTANDING ISO C header on
    * the same footing as <stdint.h> above -- it names no host kernel type and
    * no substrate idiom -- and is on the include gate's allowlist for exactly
    * that reason. The kernel branch below gets both from vms_internal.h. */
#  include <stddef.h>
#else
#  include "vms_internal.h"
#endif

/* ==========================================================================
 * 1. The shared identity vocabulary
 *
 * These four typedefs are the ONLY way a cluster identity is spelled anywhere in
 * the stack, so a CSID can never be silently passed where a SCSSYSTEMID belongs
 * (the campaign lost a day to exactly that confusion in the daemon).
 * ========================================================================== */

/*
 * SCSSYSTEMID -- the node's SCS system id, a 48-bit value SYSGEN sets and the
 * node advertises. Carried zero-extended in a uint64_t: the wire encoding
 * (6 bytes little-endian in the HELLO's logical address, a longword elsewhere)
 * is the codec's business, never a caller's.
 */
typedef uint64_t vms_scs_sysid_t;

/*
 * CSID -- the cluster system id the CLUSTER ASSIGNS during the ADD transition
 * (0x00010001, 0x00010002, ... in the lab). It is LEARNED, never chosen: CNXMAN
 * matches its own SCSSYSTEMID in the membership records to find its own. Until
 * it is learned the node is NEW and issues no DLM traffic -- so a zero CSID is
 * never "this node", it is "not yet known", and every struct that carries one
 * carries a validity flag beside it.
 */
typedef uint32_t vms_csid_t;

/*
 * Con.ID -- an SCS connection identifier. A longword: SCS$L_DST_CONID and
 * SCS$L_SRC_CONID are longwords in the published $SCSDEF definitions
 * (docs/cluster-protocol-spec.md, the LIBRARY/MACRO/EXTRACT=$SCSDEF oracle).
 */
typedef uint32_t vms_conid_t;

/* An SCS process (SYSAP) name: a fixed 16-byte blank-padded ASCII field, the
 * width GROUNDED at [62:78] and [78:94] of the connect/lookup frames
 * (spec SS4(m)/SS4(N)). NOT NUL-terminated on the wire. */
#define VMS_SCS_PROCNAME_LEN 16

/* SCSNODE: 1..6 significant characters (the VMS SYSGEN limit). Stored with room
 * for a terminator so the console/OPCOM path can print it; the WIRE field width
 * differs per frame class (8-byte blank-padded in the CM node-name field at
 * abs 90, length-prefixed in the HELLO) and belongs to the codec. */
#define VMS_SCSNODE_MAX 6

/*
 * The software-version identity this node BROADCASTS: 8 bytes, blank-padded,
 * mirroring VMS_SCS_START_SWVER_LEN (vms_cluster_codec_vc.h -- the codec owns
 * the offset, this owns the storage; vms_pe.c static-asserts the two agree).
 *
 * It is a LOADED parameter, not a constant, and deliberately so: kernel-core
 * may not hold a version literal (INV-1, tests/integration/test_identity_ssot.sh
 * -- the SSOT is src/libvms/include/ovmx_identity.h, which is USERLAND), and it
 * must never be echoed off a peer's own START body ("VMS V7.3" from a real VAX
 * is that VAX's identity, and repeating it is a masquerade). The boot carries
 * OVMX_CLUSTER_SW_VERSION down through VMS_IOCTL_SYSGEN_LOAD; until it does,
 * sw_version_len is 0 and this node advertises nothing (INV-6).
 */
#define VMS_CLUSTER_SWVER_LEN 8

/* CLUSTER_AUTHORIZE password, mirroring src/libvms/include/cluster_authorize.h's
 * CLUSTER_AUTH_PWD_LEN so the record crosses VMS_IOCTL_SYSGEN_LOAD unchanged. */
#define VMS_CLUSTER_PWD_LEN 32

/* ==========================================================================
 * 2. The SYSGEN parameters the cluster needs
 *
 * Loaded ONCE by STARTUP.EXE via VMS_IOCTL_SYSGEN_LOAD (FC-P0.10) before
 * VMS_IOCTL_CLUSTER_START, reproducing SYSBOOT's order: on VMS these are in the
 * executive before SYSINIT forms or joins, and long before the system disk is
 * mounted. Fixed-width throughout (leak table, "Word width": ILP32 VAX and LP64
 * x86_64 must lay this out identically).
 * ========================================================================== */
struct vms_cluster_params {
	/* ---- identity (fatal if absent with vaxcluster >= 1, as on VMS) ---- */
	uint8_t          scsnode[VMS_SCSNODE_MAX + 2];  /* blank/NUL padded */
	uint8_t          scsnode_len;                   /* significant chars, 1..6 */
	uint8_t          pad0;
	vms_scs_sysid_t  scssystemid;

	/* ---- membership / quorum arithmetic (FC-P3.7 reads these) ---- */
	uint16_t votes;             /* VOTES this node contributes (0 first, design D-10) */
	uint16_t expected_votes;    /* EXPECTED_VOTES */
	uint16_t qdskvotes;         /* QDSKVOTES */
	uint16_t recnxinterval;     /* RECNXINTERVAL, seconds */
	uint16_t timvcfail;         /* TIMVCFAIL, in its SYSGEN unit */
	uint16_t cluster_credits;   /* CLUSTER_CREDITS: receive buffers REQUESTED
				     * per circuit (p. 2-43). What a START body
				     * advertises is what the port's ledger could
				     * actually grant, never this -- vms_pe_fsm.h
				     * SS4b. */

	/* ---- roles ---- */
	uint8_t  vaxcluster;        /* 0 = never, 1 = when a cluster is present, 2 = always */
	uint8_t  lockdirwt;         /* LOCKDIRWT; 0 = never a directory node (D-DLM-1) */
	uint8_t  alloclass;         /* ALLOCLASS, for $n$DUAn naming */
	uint8_t  mscp_load;         /* MSCP_LOAD */
	uint8_t  mscp_serve_all;    /* MSCP_SERVE_ALL */
	uint8_t  pad1[3];

	uint32_t niscs_max_pktsz;   /* clamped to the interface MTU by the port */

	/* ---- DISK_QUORUM (the quorum disk's device name; empty = none) ---- */
	uint8_t  disk_quorum[16];
	uint8_t  disk_quorum_len;
	uint8_t  pad2;

	/*
	 * ---- CLUSTER_AUTHORIZE (group + password) ----
	 * `auth_valid` is 0 until the record is loaded; the port driver NEVER
	 * substitutes a default. What the HELLO carries in its credential field
	 * is an OPEN QUESTION (design SS5.3): the strawman daemon shipped a
	 * REPLAYED CAPTURE CONSTANT, which is a fabrication-class crutch, and
	 * FC-P0.13 measures on a clone cluster whether a real VAX opens a channel
	 * to a HELLO carrying zero. Nothing in this struct decides that; it
	 * records what the operator configured, honestly, and the answer to
	 * "what goes on the wire" arrives with FC-P0.13.
	 */
	uint16_t auth_group;
	uint8_t  auth_password[VMS_CLUSTER_PWD_LEN];
	uint8_t  auth_password_len;
	uint8_t  auth_valid;

	/*
	 * ---- this node's OWN software identity (not a SYSGEN parameter) ----
	 * The token STARTUP.EXE read from the identity SSOT and handed down
	 * (see VMS_CLUSTER_SWVER_LEN above). `sw_version_len` 0 means no boot
	 * has supplied one; cluster_sysgen_sw_version() then asserts nothing
	 * and the port counts the omission, rather than inventing a version.
	 */
	uint8_t  sw_version[VMS_CLUSTER_SWVER_LEN];
	uint8_t  sw_version_len;
	uint8_t  pad3[3];
};

/* ==========================================================================
 * 3. The node's cluster state
 *
 * What VMS_IOCTL_CLUSTER_START reports back to STARTUP.EXE, and what
 * $GETSYI CLUSTER_MEMBER projects. Deliberately coarse: the fine-grained state
 * lives in the CLUB and in each CSB's ten connectivity states, and is read
 * through the snapshot views, not through this enum.
 * ========================================================================== */
enum vms_cluster_state {
	VMS_CLUSTER_OFF        = 0,  /* VAXCLUSTER=0, or CLUSTER_START not called */
	VMS_CLUSTER_PORT_UP    = 1,  /* PEA0: open, HELLOs going out, not yet joined */
	VMS_CLUSTER_JOINING    = 2,  /* a join is in flight ("waiting to form or join") */
	VMS_CLUSTER_MEMBER     = 3,  /* the CLUB says this node is a member */
	VMS_CLUSTER_STANDALONE = 4,  /* no cluster present and VAXCLUSTER != 2 */
	VMS_CLUSTER_STATE__COUNT
};

/* ==========================================================================
 * 4. CLUB and CSB -- the connection manager's data model (FC-P3.6)
 *
 * GROUNDING. These are the two structures *VAXcluster Principles* (Davis 1993)
 * SS7.9 names, and every field below cites the page that describes it. The
 * transcript is host-only and copyrighted: page cites only, never text.
 *
 *   CSB (Cluster System Block), p. 7-23: "Associated with each VMS system in a
 *   VAXcluster configuration is a CSB", holding that system's VOTES,
 *   EXPECTED_VOTES and QDSKVOTES for the quorum algorithm, its LOCKDIRWT (the
 *   connection manager rebuilds the lock directory weight vector), a set of
 *   status flags, and the state of the SCS connection between the local
 *   SYS$CLUSTER and the SYS$CLUSTER in that system -- the TEN connectivity
 *   states, pp. 7-23/7-24. SHOW CLUSTER's MEMBERS class comes from the CSBs
 *   (p. 7-24).
 *
 *   CLUB (Cluster Block), p. 7-26: what pertains to the cluster AS A WHOLE --
 *   total votes from the current members, the number of members, computed
 *   expected votes and quorum, quorum-disk votes, the time the cluster was
 *   formed and the time of the last state transition; plus the transition
 *   working set (coordinator identity, phase, and the PROPOSED data cells,
 *   p. 7-48, which are ignored outside a transition and copied to the effective
 *   cells only if it is not abandoned, p. 7-49). All CSBs hang off the CLUB
 *   (Figure 7-4, p. 7-28), and the CLUB also holds the local system's CSB.
 *   SHOW CLUSTER's CLUSTER class comes from the CLUB (p. 7-26).
 *
 * WHY THEY LIVE HERE AND NOT BEHIND struct vms_cnxman. They are the node's
 * cluster DATA MODEL, not one layer's private working state: $GETSYI, SHOW
 * CLUSTER, the quorum arithmetic (FC-P3.7) and the DLM's rebuild (P5) all read
 * them, exactly as SYS$CLUSTER's CLUB is system-wide on VMS. The connection
 * manager's own FSM contexts (join, barrier, coordinator) stay opaque.
 *
 * INV-6 THROUGHOUT. Every value a PEER advertises carries a `_valid` companion
 * and is honestly absent until a real record supplies it. A zero CSID is "not
 * yet learned", never "node zero"; a zero LOCKDIRWT is not asserted until
 * FC-P3.2 pins which byte carries it. Nothing here has a default.
 * ========================================================================== */

/*
 * CSB status flags (p. 7-23: "a set of flags reflecting various forms of status
 * information about the system with which it is associated", enumerating
 * cluster-membership status, quorum-disk name agreement, the peer's
 * CLUSTER_SHUTDOWN notification, and whether the CSB is the local node).
 * MEMBER/SELECTED/STATUS_RCVD are also the spelling SDA prints for a CSB
 * (design SS3.4), so a lab comparison is a string match.
 */
#define VMS_CSB_F_MEMBER      0x0001u  /* member of the LOCAL cluster (p. 7-23) */
#define VMS_CSB_F_SELECTED    0x0002u  /* selected for the cluster (p. 7-49) */
#define VMS_CSB_F_STATUS_RCVD 0x0004u  /* a status message from it has arrived */
#define VMS_CSB_F_SHUTDOWN    0x0008u  /* it invoked CLUSTER_SHUTDOWN (p. 7-49) */
#define VMS_CSB_F_QDISK_AGREE 0x0010u  /* agrees on the quorum-disk name (p. 7-23) */
#define VMS_CSB_F_LOCAL       0x0020u  /* the CSB IS the local node (p. 7-23) */
#define VMS_CSB_F_REMOVED     0x0040u  /* removed from the local cluster (p. 7-23) */

/*
 * The membership bitmap the transition messages carry. Its width on the wire is
 * UNDETERMINED (design SS3.4: "store >= 32 slots and reconcile"), so the CLUB
 * keeps 128 slots and records how many the cluster has actually spoken about.
 * Defined HERE, beside the CLUB that holds the bitmap, and re-used by
 * vms_cluster_snapshot.h's view of it (which includes this header).
 */
#define VMS_CLUB_BITMAP_SLOTS 128
#define VMS_CLUB_BITMAP_WORDS (VMS_CLUB_BITMAP_SLOTS / 32)

/*
 * How many CSBs the CLUB can hold. VMS reaches a CSB from a CSID through the
 * Cluster System Vector (p. 7-25: the low 16 bits of the CSID index the CSV,
 * entry 0 is never used, entries are handed out round-robin and the high 16 bits
 * are a reuse sequence number). OVMX does NOT model the CSV: building one means
 * ASSIGNING CSIDs, and this node learns its own CSID from the cluster and never
 * assigns anybody's (design SS3.4). A flat table walked by SCSSYSTEMID or CSID is
 * what a node that only ever LEARNS needs, and 96 is the cluster scale the book
 * contemplates ("30, 40, or even 96 systems", p. 7-13) -- the same bound the
 * retired vms_cluster_members[96] mirror used, so no readback shrinks.
 */
#define VMS_CLUB_MAX_CSB 96

/*
 * One CSB: the connection manager's block for ONE system, local or remote.
 * Allocated by cnxman_club_alloc_csb() (vms_cnxman_csb.h) when a connection
 * manager is first discovered; the state machine there walks `state` through the
 * ten p. 7-23/7-24 connectivity states.
 */
struct vms_csb {
	uint8_t  in_use;          /* 0 = a free slot, not "a CSB for system 0" */
	uint8_t  state;           /* enum vms_cnxman_csb_state, pp. 7-23/7-24 */
	uint8_t  scsnode_len;     /* significant characters in scsnode[] */
	uint8_t  pad0;
	uint16_t flags;           /* VMS_CSB_F_*, p. 7-23 */
	uint16_t pad1;

	/* ---- identity ---- */
	vms_csid_t      csid;        /* ASSIGNED BY THE CLUSTER; see csid_valid */
	uint8_t         csid_valid;  /* 0 = not learned yet. NOT "csid 0" */
	uint8_t         sysid_valid; /* 0 until a real record carried the sysid */
	uint8_t         scsnode[VMS_SCSNODE_MAX + 2];
	vms_scs_sysid_t sysid;       /* the system's SCSSYSTEMID */

	/* ---- the quorum-algorithm parameters the CSB carries (p. 7-23) ---- */
	uint16_t votes;             /* VOTES */
	uint16_t expected_votes;    /* EXPECTED_VOTES */
	uint16_t qdskvotes;         /* QDSKVOTES */
	uint8_t  params_valid;      /* 0 until the peer's PARAMS record arrived */
	uint8_t  lockdirwt;         /* LOCKDIRWT: the CM rebuilds the weight vector */
	uint8_t  lockdirwt_valid;   /* 0 until FC-P3.2 pins the wire byte */
	uint8_t  pad2[3];

	/* ---- the SCS connection this CSB's state describes (p. 7-23) ---- */
	uint32_t sw_version;        /* software version as advertised, 0 if unknown */
	/* Our VMS$VAXcluster CDT to this CM. Written ONLY by
	 * cnxman_csb_bind_connection() (vms_cnxman_csb.h), because adopting a
	 * connection and restarting this block's dialogue counters on it are the
	 * same event (E77, see cm_dialogue_conid below). */
	uint32_t cdt_conid;
	uint64_t incarnation;       /* the peer's incarnation (spec SS4(i).B) */
	uint32_t last_status_ms;    /* ops.now_ms of the last CM message from it */

	/*
	 * ---- reconnect state (p. 7-23: CNXMAN "is also responsible for
	 * performing reconnect attempts if any of those SCS connections are
	 * lost"; the timing rules are p. 7-30) ----
	 * All three stamps are in the injected millisecond clock's units and are
	 * compared wrap-safely; none is meaningful unless `state` is WAIT or
	 * RECONNECT or REACCEPT.
	 */
	uint32_t remote_port_secs;   /* the number the REMOTE CM supplies (p. 7-30) */
	uint8_t  remote_port_valid;  /* 0 = not supplied; the local value stands alone */
	uint8_t  pad3[3];
	uint32_t lost_ms;            /* when connectivity was lost */
	uint32_t deadline_ms;        /* lost_ms + the p. 7-30 reconnect period */
	uint32_t next_attempt_ms;    /* the once-a-second beat's next due time */
	uint32_t attempts;           /* reconnect attempts issued for this break */
	uint32_t reconnects;         /* breaks this CSB recovered from */
	uint32_t transitions_proposed; /* transitions THIS CSB's loss caused us to propose */

	/*
	 * ---- the SYSAP dialogue counters (design sec 3.2.4 ruling E1) ----
	 * This node's own body[0:8] state for the `VMS$VAXcluster` SYSAP
	 * dialogue with THIS remote connection manager: the send/ack message
	 * numbers and the per-dialogue transaction id and correlation token.
	 * cnxman_envelope_stamp() (vms_cnxman_csb.h) is the ONLY code that
	 * reads these to fill a wire body, and FC-P3.8's glue is the only code
	 * that will advance them on a real send -- a freshly allocated CSB has
	 * genuinely sent nothing yet, so zero here is the honest starting
	 * state (INV-6), not a placeholder.
	 */
	uint16_t cm_send_msg;
	uint16_t cm_ack_msg;
	uint16_t cm_txn;
	uint16_t cm_token;

	/*
	 * ---- WHICH CONNECTION those two counters describe (E77) ----
	 *
	 * A send-msg#/ack-msg# pair is a fact about ONE SCS connection, not about
	 * a system: spec sec 4(j) grounds send-msg# as "starts at 1 on the first VC
	 * message", and the golden wire shows a node that is at send-msg# 15880
	 * on one connection open its NEXT one at 1 with ack 0
	 * (vax3-2to3-established-join-20260730: 08:00:2b:78:56:b9 holds
	 * 3551000a/a4980009 at 21078 and opens 18e3000a/a498000d at 1;
	 * formation-ci1: the SAME station pair's second dialogue
	 * 3359000a/63080008 opens at 1 after 17541 messages on the first).
	 *
	 * So the counters above belong to `cm_dialogue_conid` and to nothing
	 * else, exactly as `cm_advert_conid` above scopes the advertisement mask.
	 * cnxman_csb_bind_connection() (vms_cnxman_csb.h) is the ONLY writer of
	 * this field and of `cdt_conid`, and it restarts the dialogue whenever
	 * the connection changes. Carrying a counter across a teardown made this
	 * node OPEN a fresh Con.ID at send-msg# 8 (and 13, after refusals burned
	 * numbers on the connection that died), acking a peer message the peer
	 * had never sent on it -- and both real VAXes answered that envelope with
	 * a fatal CNXMGRERR bugcheck, 1.2 ms and 0.2 ms after the burst
	 * (integration note E76/E77).
	 *
	 * `cm_dialogue_resets` counts how many times a LIVE dialogue was
	 * discarded because the connection changed under it (the first bind, off
	 * Con.ID 0, starts a dialogue rather than replacing one and is not
	 * counted). Instrumentation only, in the
	 * block rather than a global (design sec 3.9 rule 3): connection churn is
	 * the condition this defect lived in, so it must be visible without a
	 * capture.
	 */
	uint32_t cm_dialogue_conid;
	uint32_t cm_dialogue_resets;

	/*
	 * ---- what this node has ADVERTISED about ITSELF on the connection it
	 * holds to this system RIGHT NOW (E73) ----
	 *
	 * The cat-0x01 op-0x14 MODEL and op-0x01 PARAMS pair is a PER-PEER
	 * obligation, not a step of one join: on the reference join
	 * (vax3-2to3-established-join-20260730) the joiner sent them to VAX1 at
	 * t+29.8253 AND to VAX2 at t+30.3692, each on that peer's own VC with
	 * its own send-msg# starting at 1, and both members sent theirs back the
	 * same way. A member whose CSB for this node never received them holds
	 * no parameters for it -- no VOTES -- and cannot count it.
	 *
	 * `cm_advert_conid` is the CONNECTION the mask describes, so nothing
	 * has to reset it: when the executive's Con.ID for this system changes,
	 * whatever was said down the old connection was not said down the new
	 * one and the mask is simply stale (the same per-connection rule the
	 * join applies to its own `burst_on_conn`). A lifetime counter cannot
	 * answer that question and reading one as "already advertised" is how a
	 * re-offer silently stops happening after a reconnect (E71).
	 */
	uint32_t cm_advert_conid;
	uint8_t  cm_advert_sent;    /* CNXMAN_JOIN_B_* bits, per that Con.ID  */
	uint8_t  pad4[3];
};

/*
 * How many entries the Lock Directory Weight Vector can hold (FC-P4.3).
 *
 * The book fixes the vector's CONTENTS (one entry per LOCKDIRWT unit, one per
 * system when every LOCKDIRWT is 0) but names no ceiling, so this is an OVMX
 * storage bound and is labelled as one. It is >= VMS_CLUB_MAX_CSB, so the
 * all-zero-weight case -- one entry per system, the lab's likely configuration
 * -- always fits at the full 96-system cluster scale the book contemplates.
 * A weighted set whose entries exceed it is REFUSED and counted, never
 * truncated: a truncated vector is a vector with a different modulus, i.e. a
 * different directory node for most names, which is the cluster-breaking
 * failure this whole item exists to make impossible.
 */
#define VMS_LDWV_MAX_ENTRIES 512u

/*
 * THE LOCK DIRECTORY WEIGHT VECTOR (FC-P4.3; Davis pp. 6-31..6-33, 7-40..7-42;
 * docs/research-dlm-directory-algorithm.md SS1).
 *
 * A root resource's DIRECTORY NODE is found by dividing the resource name's
 * 16-bit hash by the number of entries in this vector; the remainder indexes
 * it, and the entry names the directory node's CSID (p. 6-31). Each system
 * occupies as many CONTIGUOUS entries as its LOCKDIRWT; if LOCKDIRWT is 0 on
 * every member the vector holds exactly ONE entry per system (p. 6-32). Every
 * member's copy has the same width and the same system at each offset --
 * "logically equivalent" -- and differs only in that a system's OWN entries
 * read 0 in its own copy (p. 6-32, Fig. 6-18 p. 6-33). So a 0 entry means
 * "this node is the directory for that index", never "system zero".
 *
 * The vector is rebuilt at every state transition that can change it (p. 6-33):
 * its size is adjusted in Phase 1 and it is FILLED at Phase 2 from the
 * committed membership, before the synchronised rebuild (pp. 7-41/7-42). While
 * it is invalid -- between those two points, and before the first commit --
 * NOTHING may be resolved through it: `generation` changes on every such
 * event, which is how every cached `rsb->dir_csid` in the lock engine is
 * invalidated at once (vms_dlm_ldwv.h SS4).
 *
 * INV-6. Every entry is a CSID this node LEARNED from a real membership record,
 * placed at an offset computed from a LOCKDIRWT this node LEARNED from a real
 * parameters record. There is no default weight and no placeholder CSID: a
 * member set this node cannot weigh does not produce a partial vector, it
 * produces a refusal (vms_dlm_ldwv.h SS3).
 */
struct vms_ldwv {
	uint32_t n;            /* entries in use; 0 = no vector at all        */
	uint32_t generation;   /* bumped on EVERY change, incl. invalidation  */
	uint8_t  valid;        /* 0 = not authoritative; resolve nothing      */
	uint8_t  weights_learned; /* 0 = built on the all-zero reading, because
				   * no member has advertised a LOCKDIRWT yet
				   * (FC-P3.2 pins the wire field). Recorded so a
				   * diagnostic can say which reading it rests on,
				   * rather than the fact being invisible. */
	uint8_t  n_members;    /* systems represented, for the diagnostics    */
	uint8_t  pad;
	uint32_t entry[VMS_LDWV_MAX_ENTRIES];  /* CSIDs; own entries read 0   */
};

/*
 * The CLUB. One per node (p. 7-26), embedded in struct vms_cluster below.
 */
struct vms_club {
	/* ---- this node's own identity within the cluster ---- */
	vms_csid_t local_csid;       /* LEARNED from the membership records */
	uint8_t    local_csid_valid; /* 0 = still NEW; issues no DLM traffic */
	uint8_t    shutdown;         /* the CLUB's SHUTDOWN flag (p. 7-49) */
	uint8_t    quorum_lost;      /* CEVOTES < QUORUM right now (FC-P3.7 sets) */
	uint8_t    pad0;
	int32_t    local_csb;        /* index of the local system's CSB, -1 = none */

	/* ---- effective quorum data (p. 7-26/7-49). FC-P3.7 computes these;
	 * FC-P3.6 does not write them, so nothing here is a fabricated zero
	 * standing in for arithmetic that has not run. ---- */
	uint32_t cluster_nodes;      /* members = CSBs with SELECTED set (p. 7-49) */
	uint16_t cevotes;            /* total votes from the current members */
	uint16_t quorum;             /* cluster quorum */
	uint16_t expected_votes;     /* computed expected votes */
	uint16_t qdisk_votes;        /* votes assigned to the quorum disk */

	/* ---- proposed data cells (p. 7-48): written during a transition,
	 * ignored outside one, copied to the effective cells above at Phase 2
	 * and discarded if the transition is abandoned. ---- */
	uint16_t proposed_cevotes;
	uint16_t proposed_quorum;
	uint16_t proposed_qdisk_votes;
	uint16_t proposed_members;
	uint8_t  proposed_valid;     /* 0 outside a transition */
	uint8_t  pad1[3];

	/*
	 * ---- this node's half of p. 7-30's reconnect period ----
	 * RECNXINTERVAL, in seconds, copied from the SYSGEN parameters at
	 * cnxman_club_init() so the CSB ladder can size a reconnect window
	 * without reaching back out of the CLUB. `recnxinterval_defaulted` is 1
	 * when SYSGEN carried no value and the published OpenVMS default stood
	 * in -- recorded rather than hidden, so a diagnostic can say so.
	 */
	uint16_t recnxinterval;
	uint8_t  recnxinterval_defaulted;
	uint8_t  pad4;

	/* ---- times (p. 7-26) ---- */
	uint64_t ftime;              /* when the cluster was formed, VMS absolute */
	uint64_t fsysid;             /* the founding member's SCSSYSTEMID */
	uint8_t  ftime_valid;
	uint8_t  fsysid_valid;
	uint8_t  pad2[2];
	uint32_t last_transition_ms; /* when the last state transition occurred */

	/* ---- the transition in progress (p. 7-26: coordinator identity, the
	 * current phase) ---- */
	uint8_t    transition_active;
	uint8_t    transition_class;     /* enum vms_cnxman_transition_class */
	uint8_t    barrier_step;         /* 0..12 of the 12-step barrier */
	uint8_t    coordinator_valid;    /* 0 = no coordinator identified yet */
	uint8_t    we_coordinate;        /* nonzero iff THIS node drives it */
	uint8_t    pad3[3];
	vms_csid_t coordinator_csid;
	uint32_t   epoch;
	uint32_t   outstanding_rebuild;  /* op-0d records still unanswered */
	uint32_t   reformations;         /* transitions this node has seen */

	/* ---- the membership bitmap as the wire delivered it ---- */
	uint32_t bitmap[VMS_CLUB_BITMAP_WORDS];
	uint32_t bitmap_slots_seen;

	/*
	 * OVMX instrumentation, not a VMS field: how many CSB events the ten-
	 * state ladder ignored because the published description names no such
	 * edge. Counted rather than guessed (design SS3.9 rule 3 forbids a
	 * global to hold it), and a rising count in the lab is a question for a
	 * capture.
	 */
	uint32_t csb_ignored_events;

	/* ---- the DLM directory (FC-P4.3) ---- */

	/*
	 * The Lock Directory Weight Vector, rebuilt at every transition that can
	 * change it. See struct vms_ldwv above; the behaviour is
	 * vms_dlm_ldwv.h.
	 */
	struct vms_ldwv ldwv;

	/*
	 * THE ORDER SELF-CHECK (docs/research-dlm-directory-algorithm.md SS1/SS4).
	 * The book fixes that each system's entries are contiguous and that the
	 * offsets agree cluster-wide, but NOT the order in which systems are laid
	 * out; OVMX assumes Cluster System Vector index order (the low 16 bits of
	 * the CSID, p. 7-25). That hypothesis is self-checking from real traffic:
	 * every directory lookup this node RECEIVES must index one of this node's
	 * OWN entries. `dir_lookup_misaddressed` counts the ones that do not --
	 * a sustained count falsifies the order (or says the vector is stale) and
	 * is an alarm, never something to serve silently.
	 */
	uint32_t dir_lookups_received;
	uint32_t dir_lookup_misaddressed;

	/*
	 * Transitions at which the vector could NOT be built from the committed
	 * membership, and why (vms_dlm_ldwv.h SS3): the weighted set overflowed
	 * VMS_LDWV_MAX_ENTRIES, or some members had advertised a LOCKDIRWT and
	 * others had not, which would put every entry after the unknown member at
	 * the wrong offset. Both leave the vector INVALID rather than wrong.
	 */
	uint32_t ldwv_build_refused;

	/* ---- the CSB table (Figure 7-4: all CSBs hang off the CLUB) ---- */
	uint32_t       n_csb;        /* high-water: slots 0..n_csb-1 may be in use */
	struct vms_csb csb[VMS_CLUB_MAX_CSB];
};

/* ==========================================================================
 * 5. The per-node context
 *
 * Layer contexts are OPAQUE here: vms_cluster.h is included by every layer, so
 * exposing one layer's struct would let another reach into it. Each layer's own
 * header declares its accessors; nobody dereferences a neighbour. The CLUB is
 * the deliberate exception and is NOT a layer context -- see section 4.
 * ========================================================================== */
struct vms_pe;
struct vms_scs;
struct vms_cnxman;
struct vms_dlm_scs;
struct vms_mscp_srv;
struct vms_mscp_cl;
struct vms_cluster_fork;

struct vms_cluster {
	/*
	 * The single serializer -- VMS's fork IPL -- is NOT a field here. It is
	 * exec_mutex_t, a SEAM type (family SS7), and this struct is included by
	 * every pure header in the stack (vms_pe.h, vms_scs.h, vms_cnxman.h,
	 * vms_dlm_scs.h) down to the host unit tests and the N-node simulator,
	 * which build with NO kernel headers. Naming exec_mutex_t here would
	 * force this header to include exec_kbackend.h, which is exactly the
	 * leak the "THIS HEADER DELIBERATELY DOES NOT INCLUDE exec_kbackend.h"
	 * paragraph above rules out.
	 *
	 * The mutex lives inside the opaque `struct vms_cluster_fork *fork`
	 * below instead (FC-P0.5 defines it) -- it is glue state, and glue is
	 * exactly what vms_cluster_fork.c, not this header, is for. Held by the
	 * cluster fork thread for the whole of each event it dispatches, and by
	 * a reader taking a snapshot. Lock order (design SS3.3), never
	 * inverted:
	 *
	 *     cl->fork's mutex  ->  res->lock  ->  vms_lock_id_lock
	 *
	 * The fork thread takes the lock manager's locks like any other caller;
	 * no lock-manager path ever takes the fork mutex.
	 */

	struct vms_cluster_params params;  /* SYSGEN + CLUSTER_AUTHORIZE (FC-P0.10) */

	/*
	 * 0 until cluster_sysgen_load() COMMITTED a real parameter record.
	 *
	 * Not a redundant copy of "params is nonzero": every field in `params`
	 * has a legitimate zero (CLUSTER_CREDITS 0 grants the peer nothing, and
	 * that is a configuration, not an absence), so without this flag a
	 * reader cannot tell a configured 0 from a boot that never loaded
	 * anything -- and a port that guesses is exactly the fabrication INV-6
	 * forbids. Set ONLY here, by the executive's own commit; deliberately
	 * NOT a field of VMS_IOCTL_SYSGEN_LOAD, so no caller can assert it.
	 */
	uint8_t  params_valid;
	uint8_t  pad_pv[3];

	enum vms_cluster_state state;

	/*
	 * The Cluster Block (SS4). One per node, as on VMS -- the connection
	 * manager maintains it, but $GETSYI, SHOW CLUSTER, the quorum arithmetic
	 * and the DLM's rebuild all read it, so it is per-node context and not
	 * struct vms_cnxman's private state. Zeroed at allocation and made a
	 * CLUB by cnxman_club_init(), which is what creates the local CSB.
	 */
	struct vms_club club;

	/*
	 * The host interface name the port is bound to -- the name the SS11
	 * primary-netdev lookup reported for ETH0: (device-native naming: VMS
	 * tracks the native kernel name, and the host name NEVER surfaces to a
	 * VMS program). This string is the ONLY netif identity the core holds:
	 * the binding resolves it to its own handle inside exec_lan_open, so no
	 * struct net_device / struct ifnet is ever named up here (leak table,
	 * "Netif identity").
	 */
	uint8_t  ifname[32];

	/* Per-layer contexts, allocated at CLUSTER_START, freed at stop. */
	struct vms_cluster_fork *fork;   /* FC-P0.5 */
	struct vms_pe           *pe;     /* FC-P0.9 */
	struct vms_scs          *scs;    /* FC-P2.4 */
	struct vms_cnxman       *cnxman; /* FC-P3.8 */
	struct vms_dlm_scs      *dlm;    /* FC-P4.x */
	/*
	 * The MSCP disk SERVER (FC-P6.3). NULL is a real, common configuration
	 * and not a missing layer: a node with MSCP_LOAD=0, MSCP_SERVE_ALL=0 or
	 * simply no mounted volume serves no disks, and the published
	 * description makes serving a ROLE rather than a membership
	 * requirement. Nothing above may read "cl->mscp == NULL" as an error.
	 */
	struct vms_mscp_srv     *mscp;   /* FC-P6.3 */
	/*
	 * The MSCP disk CLASS DRIVER (FC-P7.1). NULL is a real, common
	 * configuration and not a missing layer: a node with no cluster member
	 * serving disks mounts none, and the published description makes
	 * MOUNTING a served disk a choice rather than a membership
	 * requirement. Nothing above may read "cl->mscp_cl == NULL" as an
	 * error.
	 */
	struct vms_mscp_cl      *mscp_cl; /* FC-P7.1 */
};

#endif /* OVMX_VMS_CLUSTER_H */
