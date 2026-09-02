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
	uint16_t cluster_credits;   /* CLUSTER_CREDITS */

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
 * 4. The per-node context
 *
 * Layer contexts are OPAQUE here: vms_cluster.h is included by every layer, so
 * exposing one layer's struct would let another reach into it. Each layer's own
 * header declares its accessors; nobody dereferences a neighbour.
 * ========================================================================== */
struct vms_pe;
struct vms_scs;
struct vms_cnxman;
struct vms_dlm_scs;
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

	enum vms_cluster_state state;

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
};

#endif /* OVMX_VMS_CLUSTER_H */
