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

	/*
	 * OVMX_CLEAN_DEPART (rd vms-abd) -- the WIRE-VISIBLE KILL SWITCH for the
	 * clean cluster departure. 1 (the default) = on a VMS_IOCTL_CLUSTER_STOP
	 * this node announces its departure at the SCS layer, a symmetric
	 * DISCONNECT_REQ per open connection, the way a real VMS node leaving
	 * through SHUTDOWN.COM does. 0 = it does not, and the survivors fall back
	 * on the PE last gasp and their own RECNXINTERVAL timers.
	 *
	 * IT IS NOT A PUBLISHED DEC SYSGEN PARAMETER and the name says so. VMS has
	 * no such knob because VMS has no build without the behaviour; this exists
	 * because the departure is a change to what OVMX puts on a live cluster's
	 * wire, and every such change gets a switch that turns it off in the field
	 * without a rebuild. Calling it CLEAN_DEPART would have implied a
	 * parameter an operator could look up in the VMS documentation (INV-0).
	 *
	 * DEFAULT-ON IS THE ZERO-FILLED CASE, deliberately. The ioctl carries the
	 * negation (`clean_depart_off`, vms_ioctl.h) so a caller that zero-fills
	 * the args struct -- every existing one -- gets the FAITHFUL behaviour, and
	 * only an operator who wrote OVMX_CLEAN_DEPART = 0 into OVMXVMSSYS.PAR gets
	 * the other one. The executive stores the POSITIVE sense, because that is
	 * what every reader here asks ("may I announce?").
	 */
	uint8_t  clean_depart;
	uint8_t  pad1[2];

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
 * yet learned", never "node zero"; a peer's LOCKDIRWT is absent until its
 * PARAMS record carries it (body[26:28], rd vms-fcb). Nothing here has a
 * default.
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
 * How many "systems this node has given up on" records the CLUB carries
 * (rd vms-0f9). Not VMS_CLUB_MAX_CSB: the set is not "every system in the
 * cluster", it is "every system this node has given up on AND not yet seen
 * re-incarnate", which empties itself. Sixteen is a storage bound and is
 * labelled as one -- an overflow is COUNTED and the connect is ACCEPTED.
 */
#define VMS_CLUB_MAX_GIVEUP 16

/*
 * One give-up record: this node stopped dealing with system `sysid` while it
 * was advertising incarnation `incarnation`. Both halves are required -- a
 * record with no incarnation could not tell the old incarnation from the new
 * one, which is the whole question -- so there is no "valid" flag for the
 * incarnation separately from `in_use`.
 */
struct vms_club_giveup {
	uint64_t        incarnation;
	vms_scs_sysid_t sysid;
	uint8_t         in_use;
	uint8_t         pad0[7];
};

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
	uint8_t  lockdirwt_valid;   /* 0 until the peer's PARAMS carried it */
	uint8_t  pad2;
	/* The quorum the system's own EXPECTED_VOTES gives, (EV + 2) / 2, as
	 * its PARAMS advertised it (rd vms-f297, VMS_OFF_CM_PQUORUM). Learned
	 * with params_valid; the local block computes its own. */
	uint16_t adv_quorum;
	/* The count the system's own op-0x02 carried at body[36:40] (rd
	 * vms-f297, VMS_OFB_CM_CONFIG_COUNT) -- its statement, repeated in the
	 * open that admits it. Valid once an op 0x02 from it was read. */
	uint32_t cfg_count;
	uint8_t  cfg_count_valid;
	uint8_t  pad_cfg[3];

	/* ---- the SCS connection this CSB's state describes (p. 7-23) ---- */
	uint32_t sw_version;        /* software version as advertised, 0 if unknown */
	/*
	 * THE PEER'S OWN ADVERTISED SOFTWARE VERSION (rd vms-1ee), the 8-byte
	 * token it put in its SCS formation body at abs 72 (spec SS4(g)), copied
	 * out of the port's circuit -- never inferred, never defaulted. It is
	 * the TRUST ANCHOR for "is this member running the same implementation
	 * we are": a real VAX advertises its real "VMS Vx.y" here, and anything
	 * that is not byte-identical to THIS node's own advertised token is, as
	 * far as this executive can honestly say, not OVMX.
	 *
	 * `peer_swver_len` 0 is the honest "this member has advertised nothing",
	 * which is NOT the same as "it is one of us" -- see the LDWV gate in
	 * vms_dlm_ldwv.h SS3.
	 */
	uint8_t  peer_swver[VMS_CLUSTER_SWVER_LEN];
	uint8_t  peer_swver_len;
	/*
	 * ... and the ONE derived question anybody asks of it: is that token
	 * byte-identical to the one THIS node advertises? Derived where both
	 * are in scope (cnxman_csb_set_swver) so no reader re-decides it, and
	 * so no version literal is needed anywhere (INV-1). 0 covers BOTH
	 * "advertised something else" and "advertised nothing": neither is
	 * proof, and the split-brain gate treats them the same.
	 */
	uint8_t  peer_is_ours;
	/* Our VMS$VAXcluster CDT to this CM. Written ONLY by
	 * cnxman_csb_bind_connection() (vms_cnxman_csb.h), because adopting a
	 * connection and restarting this block's dialogue counters on it are the
	 * same event (E77, see cm_dialogue_conid below). */
	uint32_t cdt_conid;
	/*
	 * THE PEER'S INCARNATION (spec SS4(i).B / SS4(g) abs 80), copied from the
	 * circuit's own formation body by cnxman_csb_set_incarnation() and by
	 * nothing else. `incarnation_valid` 0 is the honest "no START/STACK has
	 * arrived from that system yet" -- NOT "incarnation 0" (rd vms-0f9).
	 */
	uint64_t incarnation;
	uint8_t  incarnation_valid;
	uint8_t  pad5[7];
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
	/* 1 while THIS break's reconnect CONNECT is out and has not ended --
	 * set by CONNECT_SENT in [RECONNECT], and meaningful only there. */
	uint8_t  attempt_in_flight;
	uint8_t  pad3[2];
	uint32_t lost_ms;            /* when connectivity was lost */
	uint32_t deadline_ms;        /* lost_ms + the p. 7-30 reconnect period */
	uint32_t next_attempt_ms;    /* the once-a-second beat's next due time */
	uint32_t attempts;           /* reconnect attempts issued for this break */
	uint32_t reconnects;         /* breaks this CSB recovered from */
	uint32_t transitions_proposed; /* transitions THIS CSB's loss caused us to propose */
	/*
	 * ...and how many times this block's loss did NOT cause one because the
	 * cluster had never admitted the system (rd vms-b36): SELECTED clear,
	 * so p. 7-49 says there is no membership to reconfigure away. Counted
	 * in the block, beside `transitions_proposed`, because "we gave up on a
	 * system that was never in" and "we proposed its removal" are the two
	 * outcomes of the same window expiring and the difference between them
	 * is a peer bugcheck.
	 */
	uint32_t removals_withheld;
	/*
	 * How many of this CSB's own connect attempts the remote connection
	 * manager REJECTED (book p. 2-25 / correction D12: the CMs identify
	 * their version to each other in the 16-byte connect data and reject one
	 * they do not approve of). Counted in the block rather than a global
	 * (design sec 3.9 rule 3) because "the peer keeps saying no" and "the
	 * peer keeps not answering" are different diagnoses that p. 7-30's
	 * `attempts` alone cannot tell apart -- and because re-asking a peer
	 * that answered is the E81 crash-loop.
	 */
	uint32_t connect_rejects;

	/*
	 * ...and how many times the remote connection manager DISCONNECTED a
	 * connection this pair had OPEN (rd vms-dfe; SCS_CLOSE_REMOTE, the
	 * peer's own p. 2-27 DISCONNECT completing). The third diagnosis in the
	 * same family: "the peer keeps saying no", "the peer keeps not
	 * answering", and "the peer keeps hanging up on a connection it had".
	 * A rising count with `reconnects` rising beside it is a node and a peer
	 * disagreeing about whether the pair should be connected at all.
	 */
	uint32_t remote_disconnects;

	/*
	 * ...and how many once-a-second beats issued NO new attempt because this
	 * block's previous one was still in flight (rd vms-1f40). "Waiting on an
	 * answer" and "not trying" are different diagnoses, and a peer that
	 * answers slower than the beat shows up here and nowhere else.
	 */
	uint32_t attempts_held;

	/*
	 * ---- TWO CONNECTIONS FOR ONE PAIR (rd vms-1f40) ----
	 * When both ends re-dial at once, each accepts the other's CONNECT and
	 * the pair briefly holds two VMS$VAXcluster connections; the real VAX
	 * then disconnects one (measured 4/4 on the stall rig: it kept the one
	 * THIS node initiated). `attempt_conid` is this node's own outstanding
	 * reconnect CONNECT, remembered even after an accept re-binds
	 * `cdt_conid`; `alt_conid` is the pair's second OPEN connection. Both
	 * are 0 when there is nothing to remember.
	 */
	uint32_t attempt_conid;
	uint32_t alt_conid;
	uint32_t second_conns;       /* times the pair held two at once */
	uint32_t second_promotions;  /* the peer closed one; this block moved on */

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
	 * ...AND HOW OFTEN IT WAS CARRIED INSTEAD (rd vms-8c54). A
	 * re-establishment inside the p. 7-24 reconnect window, of a
	 * connection to a system the cluster still holds at the same
	 * incarnation, is the SAME conversation on a new pair: both real
	 * OpenVMS VAX V7.3 nodes in the vms-8c54 oracle continued their
	 * send-msg# across exactly that. Counted separately from the resets
	 * above so the two cases are never confused in a readback --
	 * carrying one where a reset was due is the E76/E77 crash, and this
	 * is the number that says which happened.
	 */
	uint32_t cm_dialogues_carried;
	/*
	 * ...AND WHETHER THE NEXT FRAME FROM THAT PEER STILL OWES US ITS
	 * POSITION (rd vms-1f40). Armed by cnxman_csb_bind_reconnect() and
	 * taken by the first inbound envelope on the re-established
	 * connection; `cm_resumes` counts the times that really moved the send
	 * counter back, which is the number that says a hole was prevented.
	 */
	uint8_t  cm_resume_pending;
	/*
	 * ...AND WHETHER THIS SYSTEM IS NAMED IN A STATE TRANSITION THIS NODE
	 * HAS ACKNOWLEDGED AND THAT HAS NOT YET ENDED (rd vms-eb3). Set at
	 * p. 7-41's Phase 1 -- the coordinator's own block, and every block
	 * the proposal's nodemap names -- and cleared when the transition
	 * completes or is abandoned. It is the one fact that makes a lost
	 * connection to that system a RE-ESTABLISHMENT before p. 7-42's
	 * Phase 2 has set SELECTED: measured on a real V7.3 trio, a joiner
	 * frozen between its Phase-1 answer and the GO re-established both
	 * members with its dialogue carried, and the coordinator re-sent the
	 * GO on the new connection (vms-eb3 oracle F5/F6).
	 */
	uint8_t  cm_phase1_named;
	/*
	 * ...AND WHETHER THE SYSTEM HAS COME BACK AS A NEW INCARNATION SINCE
	 * THIS BLOCK'S DIALOGUE BEGAN (rd vms-eb3). p. 7-24 DEAD / p. 7-25: the
	 * old incarnation's conversation died with it, and the new one is dealt
	 * with "just as if it were joining the cluster for the first time". Set
	 * when the circuit advertises an incarnation different from the one
	 * this block recorded; cleared when a fresh dialogue is bound.
	 */
	uint8_t  cm_new_incarnation;
	/*
	 * ...AND WHETHER THE PEER MAY STILL TURN OUT TO BE CONTINUING THE
	 * DIALOGUE THIS BLOCK JUST RESET (rd vms-ba4). A pre-admission joiner's
	 * block is not entitled to carry by csb_dialogue_may_continue(), yet a
	 * real VAX's block for that joiner survives a re-formed circuit and
	 * continues -- measured: VAX send=3 ack=2 against this node's reset
	 * send=1 ack=0, and CNXMGRERR within a millisecond. So a reset of a
	 * LIVE dialogue keeps its three numbers here, armed for one frame:
	 * the peer's first envelope on the new connection says which it is.
	 */
	uint8_t  cm_adopt_pending;
	uint16_t cm_prev_send;
	uint16_t cm_prev_txn;
	uint16_t cm_prev_token;
	/*
	 * ...AND WHAT THE PEER'S OWN CONNECT DATA SAID ABOUT THE CONNECTION
	 * BEING ACCEPTED (rd vms-ba4). content[106:108] of its CONNECT_REQ is
	 * the highest send-msg# it has TAKEN from this node (rd vms-8c54);
	 * `cm_advertised_ack` is the same cell of OUR ACCEPT_REQ. Non-zero, the
	 * peer is continuing, and the bind that follows resumes instead of
	 * resetting -- measured: the VAX dialled with 3 (or 2), this node
	 * answered and then opened at send 1 / ack 0, CNXMGRERR.
	 */
	uint16_t cm_peer_taken;
	uint16_t cm_advertised_ack;
	uint8_t  cm_peer_taken_valid;
	uint8_t  cm_adopt_pad;
	uint32_t cm_dialogues_adopted;  /* peer continued: resumed from it   */
	uint32_t cm_adopt_too_late;     /* peer continued after we had spoken*/
	uint32_t cm_resumes;

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
	/*
	 * ---- what this system has told US it is (rd vms-e88) ----
	 *
	 * `adv_members` is the cluster member count its latest op-0x01 PARAMS
	 * carried at body[18:20] -- its own count if it is a member, 0 if it
	 * belongs to no cluster -- and `adv_valid` 0 is the honest "no PARAMS
	 * from it yet", never "0 members". A joiner reads the two to decide
	 * WHOM it may ask for admission and WHEN (Davis p. 7-37, measured on
	 * real V7.3 trios): only a system that says it is a member, and only
	 * once it has connectivity with as many members as they say there are.
	 */
	uint8_t  adv_valid;
	uint16_t adv_members;
	/*
	 * ...and the member count THIS node last put in a PARAMS on
	 * `cm_advert_conid`, so a count that has changed since is said again
	 * (a real member re-sends its PARAMS to a waiting joiner when a
	 * transition changes it -- measured, e88 trio B).
	 */
	uint16_t cm_advert_members;
	uint8_t  pad4[2];
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
				   * no member had advertised a LOCKDIRWT yet
				   * (PARAMS body[26:28], rd vms-fcb). Recorded so a
				   * diagnostic can say which reading it rests on,
				   * rather than the fact being invisible. */
	uint8_t  n_members;    /* systems represented, for the diagnostics    */
	uint8_t  any_foreign;  /* 1 = a member could NOT be proven OVMX. THE
				* ALL-OVMX GATE (vms-3e3): the OVMX-own directory
				* hash (rung A", design SS3.6) is grounded ONLY when
				* this is 0. Set from the same survey that feeds the
				* split-brain gate (#1138), so the two rest on one
				* reading of the member set, never two. */
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
	/*
	 * THE ENFORCEMENT LATCH (FC-P8.1, rd vms-b6d). Set the first time this
	 * node, as a COMMITTED member whose own CSB counts, actually PERCEIVED
	 * quorum -- p. 7-4's "cluster activity proceeds while the available
	 * votes are >= QUORUM". Only from that moment on is a subsequent
	 * quorum_lost a real LOSS rather than arithmetic that has not finished:
	 * a member that has not yet learned its peers' PARAMS honestly computes
	 * QUORUM=(0+2)/2=1 over an empty vote set and shows quorum_lost=1, and
	 * freezing on THAT would hang every single join. So enforcement reads
	 * this latch, never the raw flag. Cleared only by cnxman_club_init() --
	 * a new cluster life earns its own perception of quorum.
	 */
	uint8_t    quorum_armed;
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
	/*
	 * THE LAST RECONFIGURATION (rd vms-f297): the member count and total
	 * votes the last FORMATION or REMOVAL left, as that transition's own
	 * open carried them (VMS_OFB_CM_OPEN_RC_*) or as this node founded the
	 * cluster. Every ADD open carries the pair; no ADD changes it. A node
	 * that has seen neither holds rc_valid 0, and its open carries 0 0 --
	 * what a real V7.3 member that joined later sends (XA/XE/XF ep4).
	 */
	uint8_t  rc_members;
	uint8_t  rc_votes;
	uint8_t  rc_valid;
	/*
	 * THE NEXT CSV SLOT the cluster will assign (rd vms-f297,
	 * VMS_OFB_CM_OPEN_SLOT_NEXT): slots are handed out round-robin and never
	 * reused within the cluster's life (p. 7-25), so this is one more than
	 * the highest slot ever assigned -- including a departed member's, which
	 * no CSB still shows. Learned from every open, advanced by every
	 * assignment this node makes.
	 */
	uint8_t  slot_next_valid;
	uint16_t slot_next;
	uint8_t  rc_lost;   /* a removal committed whose pair this node could
			     * not derive: no open of ours may carry one */
	uint8_t  pad2;
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

	/*
	 * OVMX instrumentation, not a VMS field (rd vms-dfe): CSBs this CLUB
	 * DEALLOCATED because the connection manager had given up on them
	 * (p. 7-25's "its old CSB is deallocated"). Nonzero means a system was
	 * released back to discovery; the port decides whether a fresh block
	 * appears for it. See cnxman_club_reclaim_abandoned().
	 */
	uint32_t csb_reclaimed;

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

	/*
	 * ---- THE GIVE-UP LEDGER (rd vms-0f9) ----
	 *
	 * p. 7-24's DEAD state is "a new incarnation of a VAX system has been
	 * seen; the CSB whose connection state is DEAD represents the OLD
	 * incarnation" -- so the executive is expected to remember WHICH
	 * incarnation of a system it has stopped dealing with, and to keep
	 * remembering it until a different one shows up.
	 *
	 * That fact cannot live in the CSB, because p. 7-25 deallocates the
	 * block and rebuilds it (rd vms-dfe, #1309) within a second of the
	 * give-up. It lives here instead, beside the CSB table it outlives.
	 *
	 * WHAT IT IS FOR. A real OpenVMS connection manager answers REJECT_REQ
	 * to an inbound VMS$VAXcluster connect for a relationship it has given
	 * up on -- measured on three real V7.3 nodes in
	 * tests/lab/captures/vms-b36-cnxmgrerr-20260925/ -- and ACCEPTS again
	 * once the peer comes back as a new incarnation. Accepting instead is
	 * what put a real VAX into CNXMGRERR 0.3 ms after the connection
	 * completed.
	 *
	 * BOUNDED, AND HONEST WHEN IT OVERFLOWS. `giveup_overflow` counts the
	 * records that did not fit; a system with no record is ACCEPTED, because
	 * this node cannot prove it gave up on that incarnation and must not
	 * refuse on a guess (INV-6).
	 */
	struct vms_club_giveup giveup[VMS_CLUB_MAX_GIVEUP];
	uint32_t giveup_overflow;    /* records that did not fit -- accepted */
	uint32_t giveup_armed;       /* records really written */
	uint32_t giveup_cleared;     /* ...cleared by a NEW incarnation */

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
