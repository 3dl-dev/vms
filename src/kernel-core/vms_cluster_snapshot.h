/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster_snapshot.h - read-only, fixed-width VIEWS of each cluster layer's
 * executive state (FC-P0.1).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.5 ("the diagnostics exist
 * so tests assert EXECUTIVE STATE (INV-6), never a frame count"), SS3.9 (the
 * three-file layer layout: `vms_<layer>_snapshot.h` is a read-only view struct
 * "diagnostics ioctls AND host tests assert on the same struct").
 *
 * WHY ONE HEADER AND NOT FIVE. The design's per-layer table names
 * `vms_<layer>_snapshot.h`; the plan's FC-P0.1 row names ONE file,
 * `vms_cluster_snapshot.h`, "fixed-width view structs per layer". They are the
 * same artifact seen from two distances, and one file is the better call: the
 * views are an ABI (they cross an ioctl to SHOW CLUSTER / SDA-shaped
 * diagnostics), the width and alignment rules below apply to all of them
 * identically, and a reviewer comparing OVMX's CLUSTER_DIAG output against SDA
 * reads one file. Each layer's block is separated below and a layer's own
 * header includes only this one.
 *
 * ---------------------------------------------------------------------------
 * THREE RULES THAT MAKE A VIEW A VIEW
 *
 * 1. EVERY FIELD NAMES THE EXECUTIVE OBJECT IT IS READ FROM. A view is a
 *    projection of a real PB/VC/CDT/CSB/CLUB/LKB, taken under the fork mutex.
 *    Nothing here is a frame count standing in for state, nothing is copied off
 *    the wire, nothing has a default (INV-6). If a field cannot be sourced, see
 *    rule 2 -- it is NOT rendered as the zero the scalar happens to hold.
 *
 * 2. ABSENT IS NOT ZERO. A value the executive has not LEARNED yet (the local
 *    CSID before the cluster assigns it; LOCKDIRWT before FC-P3.2 pins which
 *    byte carries it) travels with a `*_valid` flag, and the reader blanks the
 *    column when the flag is clear. This is the identical discipline
 *    struct exec_proc_acct's has_* flags enforce for SHOW SYSTEM, and it exists
 *    for the identical reason: a fabricated zero on a cluster identity is how a
 *    placeholder lock id bugchecked a real VAX.
 *
 * 3. NO 64-BIT SCALAR CROSSES THIS BOUNDARY. Every 64-bit quantity (SCSSYSTEMID,
 *    VMS absolute time, an incarnation quadword) is carried as an explicit
 *    `_lo`/`_hi` uint32_t PAIR, low half first. Reason: uint64_t's ALIGNMENT is
 *    not the same on both substrates -- 8 bytes on LP64 x86_64/aarch64, 4 on
 *    ILP32 VAX -- so a struct containing one can lay out differently on the two
 *    kmods that must agree on it. Splitting removes the question permanently,
 *    including for fields a later item inserts. With no 64-bit member every
 *    struct here has 4-byte alignment everywhere, so the _Static_asserts at the
 *    bottom of each block hold on both. vms_cluster_snapshot_q() recomposes a
 *    pair for a caller that wants the scalar.
 * ---------------------------------------------------------------------------
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_CLUSTER_SNAPSHOT_H
#define OVMX_VMS_CLUSTER_SNAPSHOT_H

#include "vms_cluster.h"   /* the identity vocabulary + fixed-width types */

/* Recompose a `_lo`/`_hi` view pair into the 64-bit scalar it projects. */
static inline uint64_t vms_cluster_snapshot_q(uint32_t lo, uint32_t hi)
{
	return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* ==========================================================================
 * PORT layer (vms_pe) -- what SDA SHOW PORT / SCACP SHOW CHANNEL show.
 * ========================================================================== */

/*
 * Channel states, spec SS4(a)-(c)/(k): a "channel" is one (local netif, remote
 * station) pair, and it is VERIFIED by the b2/b3/b4 handshake before the port
 * will carry a virtual circuit over it. The ladder is what the port driver
 * records per channel; FC-P0.8 owns the transition table that walks it.
 */
enum vms_pe_channel_state {
	VMS_PE_CH_CLOSED   = 0,  /* no channel object */
	VMS_PE_CH_SEEN     = 1,  /* a HELLO from this station was received */
	VMS_PE_CH_B2       = 2,  /* verify step b2 */
	VMS_PE_CH_B3       = 3,  /* verify step b3 */
	VMS_PE_CH_B4       = 4,  /* verify step b4 reached: the channel is usable */
	VMS_PE_CH_STATE__COUNT
};

/* Virtual-circuit states, spec SS4(g)/(i): START/STACK/ACK with the incarnation
 * echo, then OPEN; TIMVCFAIL closes it and the port re-forms. */
enum vms_pe_vc_state {
	VMS_PE_VC_CLOSED      = 0,
	VMS_PE_VC_START_SENT  = 1,
	VMS_PE_VC_STACK_SENT  = 2,
	VMS_PE_VC_OPEN        = 3,
	VMS_PE_VC_STATE__COUNT
};

struct vms_pe_channel_view {
	uint8_t  remote_mac[6];      /* PB channel: the remote station address */
	uint8_t  state;              /* enum vms_pe_channel_state */
	uint8_t  remote_sysid_valid; /* 0 until a HELLO carried the peer's SCSSYSTEMID */
	uint32_t remote_sysid_lo;    /* SB's SCSSYSTEMID (rule 3), when valid */
	uint32_t remote_sysid_hi;
	uint32_t last_rx_ms;         /* exec_ticks_ms of the last frame on this channel */
	uint32_t hello_tx;           /* HELLOs the port SENT on this channel */
	uint32_t hello_rx;           /* HELLOs RECEIVED from this station */
	uint32_t verified_pktsz;     /* packet size the b-ladder verified, 0 if not yet */
};
_Static_assert(sizeof(struct vms_pe_channel_view) == 32,
	       "vms_pe_channel_view is a cross-substrate ABI struct");

struct vms_pe_vc_view {
	uint32_t peer_sysid_lo;      /* the SB this VC belongs to */
	uint32_t peer_sysid_hi;
	uint8_t  state;              /* enum vms_pe_vc_state */
	uint8_t  pad0[3];
	uint32_t send_seq;           /* the VC's ONE contiguous send sequence */
	uint32_t recv_seq;           /* highest sequence received in order */
	uint32_t recv_ack;           /* cumulative ack we send the peer */
	uint32_t peer_recv_ack;      /* the cumulative ack the PEER last sent us */
	uint32_t unacked;            /* messages in the unacked ring */
	uint32_t retransmits;        /* retransmissions (same seq re-sent) */
	uint32_t incarnation_lo;     /* the incarnation quadword echoed at formation */
	uint32_t incarnation_hi;
	uint32_t timvcfail_ms_left;  /* time to the TIMVCFAIL deadline, 0 when closed */
	uint32_t credits_send;       /* port-level credit window state */
	uint32_t credits_receive;

	/*
	 * Added by FC-P1.6, additively and at the end (rule: append, never
	 * insert -- the struct above is the FC-P1.2/P1.9 layout every existing
	 * reader already decodes). Both fields are real vms_pe_fsm.c pe_vc
	 * counters (INV-6), never derived or fabricated here:
	 *
	 *   rx_gaps      pe_vc.rx_gaps -- frames discarded for arriving ahead
	 *                of recv_seq + 1, the go-back-N receiver's own count of
	 *                loss it absorbed (design SS3.2.5). Non-zero on a lossy
	 *                LAN, zero on a clean one; never a reason to break.
	 *   down_reason  pe_vc.last_down_reason -- enum pe_vc_down_reason, the
	 *                MOST RECENT reason this circuit went down, 0 meaning
	 *                "never down since this object was allocated". A VC
	 *                that re-formed after a break still reports its last
	 *                reason here; the state field (above) is what tells a
	 *                reader whether the circuit is OPEN right now.
	 */
	uint32_t rx_gaps;
	uint8_t  down_reason;
	uint8_t  pad1[3];
};
_Static_assert(sizeof(struct vms_pe_vc_view) == 64,
	       "vms_pe_vc_view is a cross-substrate ABI struct");

struct vms_pe_view {
	uint8_t  port_open;          /* exec_lan_open succeeded and PEA0: exists */
	uint8_t  hwaddr_valid;       /* 0 unless exec_lan_hwaddr returned a real MAC */
	uint8_t  hwaddr[6];          /* the interface's REAL hardware address */
	uint8_t  link_up;            /* exec_lan_link_up on the bound interface */
	uint8_t  pad0[3];
	uint32_t mtu;                /* the interface MTU that clamps the next field */
	uint32_t max_pktsz;          /* NISCS_MAX_PKTSZ after the clamp */
	uint32_t n_channels;         /* channel objects the port holds */
	uint32_t n_vcs;              /* virtual circuits the port holds */
	uint32_t rx_frames;          /* frames rx_cb delivered to the input queue */
	uint32_t rx_drops_nobuf;     /* frames dropped: the pool was empty (rule 1) */
	uint32_t rx_drops_badclass;  /* frames the codec could not classify */
	uint32_t tx_frames;          /* frames exec_lan_xmit accepted */
	uint32_t tx_errors;          /* exec_lan_xmit failures */
};
_Static_assert(sizeof(struct vms_pe_view) == 48,
	       "vms_pe_view is a cross-substrate ABI struct");

/* ==========================================================================
 * SCS layer (vms_scs) -- what SDA SHOW CONNECTIONS shows.
 * ========================================================================== */

/*
 * The CDT connection ladder (design SS3.4: "the ch.2 ladder OPEN -> DISC
 * SENT/RCVD -> MATCH -> CLOSED", plus the pre-open verbs). FC-P2.2 owns the
 * transition table; these are the states a CDT can be OBSERVED in, and they are
 * the names SDA prints, so a lab comparison is a string match.
 */
enum vms_scs_cdt_state {
	VMS_SCS_CDT_CLOSED       = 0,
	VMS_SCS_CDT_LISTEN       = 1,  /* a SYSAP is listening on this name */
	VMS_SCS_CDT_CONNECT_SENT = 2,
	VMS_SCS_CDT_CONNECT_RCVD = 3,
	VMS_SCS_CDT_ACCEPT_SENT  = 4,
	VMS_SCS_CDT_ACCEPT_RCVD  = 5,
	VMS_SCS_CDT_OPEN         = 6,
	VMS_SCS_CDT_DISC_SENT    = 7,
	VMS_SCS_CDT_DISC_RCVD    = 8,
	VMS_SCS_CDT_DISC_MATCH   = 9,
	VMS_SCS_CDT_STATE__COUNT
};

struct vms_scs_cdt_view {
	uint32_t local_conid;                        /* the CDT's own Con.ID */
	uint32_t remote_conid;                       /* the peer's, 0 until learned */
	uint8_t  remote_conid_valid;
	uint8_t  state;                              /* enum vms_scs_cdt_state */
	uint8_t  pad0[2];
	uint32_t peer_sysid_lo;                      /* the SB this CDT rides */
	uint32_t peer_sysid_hi;
	uint8_t  local_name[VMS_SCS_PROCNAME_LEN];   /* our SYSAP, blank-padded */
	uint8_t  remote_name[VMS_SCS_PROCNAME_LEN];  /* the peer's SYSAP */
	uint16_t credit_send;                        /* credits WE may spend */
	uint16_t credit_receive;                     /* credits we have EXTENDED */
	uint16_t credit_pending;                     /* extended but not yet returned */
	uint16_t pad1;
	uint32_t msgs_sent;
	uint32_t msgs_received;
};
_Static_assert(sizeof(struct vms_scs_cdt_view) == 68,
	       "vms_scs_cdt_view is a cross-substrate ABI struct");

struct vms_scs_view {
	uint32_t n_sbs;              /* system blocks: one per remote system */
	uint32_t n_cdts;             /* connection descriptors in any non-CLOSED state */
	uint32_t n_sysaps;           /* SYSAPs registered in the local directory */
	uint32_t conid_seq;          /* the Con.ID allocator's current low word (SS4(t)) */
	uint32_t conid_epoch;        /* its high word, reseeded per boot (SS4(t)) */
	uint32_t dir_lookups_served; /* SCS$DIRECTORY lookups this node ANSWERED */
	uint32_t dir_lookups_sent;   /* lookups this node ISSUED */
	uint32_t credit_stalls;      /* sends deferred for want of a credit */
};
_Static_assert(sizeof(struct vms_scs_view) == 32,
	       "vms_scs_view is a cross-substrate ABI struct");

/* ==========================================================================
 * CONNECTION MANAGER (vms_cnxman) -- what SDA SHOW CLUSTER/CSB shows.
 * ========================================================================== */

/*
 * The CSB's ten connectivity states (design SS3.4: "CSB per remote CM (ten
 * connectivity states NEW..LOCAL, flags member/selected/status_rcvd)").
 *
 * PINNED BY FC-P3.6, as the frozen contract said it would be -- and pinned from
 * the PUBLISHED DESCRIPTION, not from wire inference: *VAXcluster Principles*
 * (Davis 1993) pp. 7-23/7-24 enumerates the ten states of "the SCS connection
 * between the local SYS$CLUSTER and the SYS$CLUSTER residing in the system
 * associated with the CSB", in this order, with NEW first and LOCAL last. The
 * two ordinals the contract already froze (NEW = 0, LOCAL = 9, ten states) are
 * exactly what that enumeration yields, so no ABI value moved; the eight names
 * between them are now spelled instead of left as bare ordinals.
 *
 * Each state's one-line gloss is the book's own sense, in this file's words:
 * the transcript is copyrighted and host-only (page cites, never text).
 */
enum vms_cnxman_csb_state {
	VMS_CNXMAN_CSB_NEW        = 0,  /* just allocated: a newly discovered CM,
					 * or one that left and is returning (7-23) */
	VMS_CNXMAN_CSB_CONNECT    = 1,  /* our initial SCS CONNECT has been sent (7-24) */
	VMS_CNXMAN_CSB_ACCEPT     = 2,  /* an initial inbound CONNECT is being accepted */
	VMS_CNXMAN_CSB_OPEN       = 3,  /* the SCS connection exists: the NORMAL state */
	VMS_CNXMAN_CSB_DISCONNECT = 4,  /* an SCS DISCONNECT is in progress */
	VMS_CNXMAN_CSB_WAIT       = 5,  /* connectivity lost; a timeout is running,
					 * at whose end a reconnect is attempted */
	VMS_CNXMAN_CSB_RECONNECT  = 6,  /* a reconnect attempt is in progress */
	VMS_CNXMAN_CSB_REACCEPT   = 7,  /* we are accepting the peer's reconnect */
	VMS_CNXMAN_CSB_DEAD       = 8,  /* a NEW INCARNATION of that system has been
					 * seen; this CSB is the old incarnation */
	VMS_CNXMAN_CSB_LOCAL      = 9,  /* reserved for the LOCAL connection manager's
					 * own CSB -- never a connection subject */
	VMS_CNXMAN_CSB_STATE__COUNT = 10
};

/* Transition classes (design SS3.7, spec SS4(o)-(r)). */
enum vms_cnxman_transition_class {
	VMS_CNXMAN_TR_NONE   = 0,
	VMS_CNXMAN_TR_ADD    = 1,   /* a node joins */
	VMS_CNXMAN_TR_REMOVE = 2,   /* a node is removed by the coordinator */
	VMS_CNXMAN_TR_DEPART = 3,   /* a node departs of its own accord */
	VMS_CNXMAN_TR_CLASS__COUNT
};

struct vms_csb_view {
	uint32_t csid;                          /* ASSIGNED BY THE CLUSTER, see below */
	uint8_t  csid_valid;                    /* 0 = not yet learned; NOT "csid 0" */
	uint8_t  state;                         /* enum vms_cnxman_csb_state ordinal */
	uint8_t  is_member;                     /* CSB flag: member */
	uint8_t  is_selected;                   /* CSB flag: selected */
	uint8_t  status_rcvd;                   /* CSB flag: status received */
	uint8_t  scsnode_len;
	uint8_t  scsnode[VMS_SCSNODE_MAX + 2];  /* as the peer advertised it */
	uint16_t votes;                         /* the peer's advertised VOTES */
	uint8_t  votes_valid;                   /* 0 until a PARAMS record arrived
						  * (FC-P3.7 closes the gap FC-P3.6
						  * flagged: an un-advertised VOTES
						  * is not an advertised 0, INV-6) */
	uint8_t  lockdirwt;                     /* the peer's advertised LOCKDIRWT ... */
	uint8_t  lockdirwt_valid;               /* ... 0 until FC-P3.2 pins the byte */
	uint8_t  pad0;
	uint32_t peer_sysid_lo;                 /* the peer's SCSSYSTEMID */
	uint32_t peer_sysid_hi;
	uint32_t sw_version;                    /* software version as advertised */
	uint32_t cdt_conid;                     /* our VMS$VAXcluster CDT to this CM */
	uint32_t incarnation_lo;                /* the peer's incarnation */
	uint32_t incarnation_hi;
	uint32_t last_status_ms;                /* exec_ticks_ms of the last CM message */
};
_Static_assert(sizeof(struct vms_csb_view) == 52,
	       "vms_csb_view is a cross-substrate ABI struct");

/* The membership bitmap's width on the wire is UNDETERMINED (design SS3.4:
 * "store >= 32 slots and reconcile"), so the CLUB keeps 128 slots and the view
 * reports how many of them the cluster has actually spoken about.
 * VMS_CLUB_BITMAP_SLOTS/_WORDS moved to vms_cluster.h with FC-P3.6, beside the
 * struct vms_club that HOLDS the bitmap, so the CLUB and its view cannot drift
 * apart; the values are unchanged and this header includes that one, so every
 * consumer still sees them here. */

struct vms_club_view {
	uint32_t local_csid;                        /* LEARNED from the membership records */
	uint8_t  local_csid_valid;                  /* 0 = still NEW; issues no DLM traffic */
	uint8_t  state;                             /* enum vms_cluster_state */
	uint8_t  quorum_lost;                       /* CEVOTES < QUORUM right now */
	uint8_t  pad0;
	uint32_t epoch;                             /* the transition epoch last seen */
	uint32_t cluster_nodes;                     /* members the CSB table counts */
	uint16_t cevotes;                           /* votes present, summed from CSBs */
	uint16_t quorum;                            /* computed from EXPECTED_VOTES */
	uint16_t expected_votes;                    /* our SYSGEN value */
	uint16_t pad1;
	uint32_t bitmap[VMS_CLUB_BITMAP_WORDS];     /* membership bitmap as received */
	uint32_t bitmap_slots_seen;                 /* how many slots the wire has covered */
	uint8_t  transition_active;
	uint8_t  transition_class;                  /* enum vms_cnxman_transition_class */
	uint8_t  barrier_step;                      /* 0..12 of the 12-step barrier */
	uint8_t  coordinator_valid;                 /* 0 = no coordinator identified */
	uint32_t coordinator_csid;                  /* the CM driving the transition */
	uint32_t outstanding_rebuild;               /* op-0d records still unanswered */
	uint32_t ftime_lo;                          /* CLUSTER_FTIME (VMS absolute) */
	uint32_t ftime_hi;
	uint32_t fsysid_lo;                         /* CLUSTER_FSYSID */
	uint32_t fsysid_hi;
	uint32_t reformations;                      /* transitions this node has seen */
};
_Static_assert(sizeof(struct vms_club_view) == 76,
	       "vms_club_view is a cross-substrate ABI struct");

/* ==========================================================================
 * DLM SYSAP (vms_dlm_scs) -- the wire arm of the lock manager. What SDA
 * SHOW LOCK/RESOURCE corroborates; the LKB/RSB rows themselves are
 * CLUSTER_DIAG_LOCK's business (FC-P4.x) and project vms_lock.c objects.
 * ========================================================================== */

/* The rebuild FSM's phases (design SS3.6: freeze -> per-type rebuild -> thaw,
 * driven by the CNXMAN transition callback). */
enum vms_dlm_rebuild_phase {
	VMS_DLM_RB_IDLE   = 0,
	VMS_DLM_RB_FROZEN = 1,   /* lock activity held while the rebuild runs */
	VMS_DLM_RB_ACTIVE = 2,   /* records being exchanged */
	VMS_DLM_RB_THAW   = 3,
	VMS_DLM_RB_PHASE__COUNT
};

struct vms_dlm_scs_view {
	uint8_t  lockdirwt;             /* OUR advertised LOCKDIRWT (0 = never directory) */
	uint8_t  rebuild_phase;         /* enum vms_dlm_rebuild_phase */
	uint8_t  connected;             /* the VMS$VAXcluster CDT carrying cat-02 is open */
	uint8_t  pad0;
	uint32_t rebuild_generation;    /* bumped every transition; invalidates dir caches */
	uint32_t proxy_lkbs;            /* LKBs mastered ELSEWHERE that we hold */
	uint32_t mastered_resources;    /* RSBs this node masters */
	uint32_t directory_entries;     /* directory entries this node stores for others */
	uint32_t req_sent;              /* requests we originated (real proxy LKBs) */
	uint32_t req_received;          /* inbound requests dispatched to the engine */
	uint32_t grants_sent;           /* grants built from a real granted LKB */
	uint32_t grants_received;
	uint32_t declined;              /* honest declines: neither master nor directory */
	uint32_t rebuild_records_in;    /* op-0d records received */
	uint32_t rebuild_records_out;
};
_Static_assert(sizeof(struct vms_dlm_scs_view) == 48,
	       "vms_dlm_scs_view is a cross-substrate ABI struct");

#endif /* OVMX_VMS_CLUSTER_SNAPSHOT_H */
