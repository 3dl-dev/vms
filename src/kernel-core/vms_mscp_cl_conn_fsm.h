/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_mscp_cl_conn_fsm.h - WHEN the MSCP disk CLASS DRIVER is allowed to open
 * its `MSCP$DISK` connection to a member, and to WHICH members.
 *
 * This is admission policy only. The MSCP protocol is vms_mscp_cl_io_fsm.h;
 * the executive bindings are vms_mscp_cl.c. This file decides one thing:
 * "may VMS$DISK_CL_DRVR connect to this system, right now?" -- and it exists
 * because answering that question wrongly is what E64 measured on a real
 * 2-node VAX cluster.
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.9 (pure table-driven
 * [state][event] FSM, injected ops, injected clock, no globals).
 * Wire spec: docs/cluster-protocol-spec.md SS4(L)(1) (the joiner DRIVES) and
 * SS4(L) "Shared-sequence deadlock -- the mechanism, live-grounded".
 * Choreography: docs/design-cluster-join-choreography.md (the definitive 2->3
 * established-join reference, `vax3-2to3-established-join-20260730.pcap`).
 * Published book: *VAXcluster Principles* (Davis 1993) SS2.11, pp. 2-48..2-51
 * (the SCS Directory Service and the Process Poller: you ASK whether a node
 * hosts a SYSAP before you connect to it).
 *
 * ===========================================================================
 * THE TWO RULES, AND THE MEASUREMENT THAT PRODUCED EACH
 *
 * RULE 1 -- LOOKUP BEFORE CONNECT. A connection to `MSCP$DISK` on a member
 * may be opened only after THAT MEMBER has really answered a directory
 * inquiry about that name affirmatively. Not "we assume every VAX serves
 * disks"; not "we tried and it refused". Spec SS4(L) grounds why this is
 * load-bearing rather than cosmetic: the per-channel `send_seq` is shared
 * across every Con.ID pair, so a connect the member cannot yet process
 * occupies a slot and creates an IN-ORDER HOLE -- the member froze its
 * `recv_ack` and never accepted anything after it, including the
 * `VMS$VAXcluster` connect that admission depends on. vms_cnxman_join_fsm.h
 * states the same invariant for the JOIN's own MSCP connect and enforces it
 * by construction; this FSM is that invariant for the class driver's
 * autonomous sweep, which is a SECOND originator on the same channel.
 *
 * RULE 2 -- THE JOIN DRIVES ALONE. While this node is still joining, the
 * connection manager's join FSM is the sole originator on the channel
 * (spec SS4(L)(1): "the joiner actively DRIVES the post-START sequence").
 * The class driver's sweep therefore originates NOTHING until the node is a
 * member -- not even a directory inquiry, because an inquiry opens a real
 * SCS$DIRECTORY connection and that too takes a slot in the shared sequence.
 * This is what the reference joiner does: it completes its own directory
 * round, its MSCP$DISK connect and its VMS$VAXcluster connect against the
 * member it joins THROUGH, and only about a second later does the reciprocal
 * disk-client half against the OTHER member (2->3 reference, J->VAX2 at
 * t+1.0 s, after the admission burst on the VAX1 VC).
 *
 * E64, for the record: OVMX presented 711 `MSCP$DISK` CONNECT_REQ frames and
 * ZERO directory inquiries of its own to one of the two VAXes, because this
 * sweep ran from CLUSTER_START on a one-second beat with neither gate. The
 * FIRST sequenced frame OVMX ever sent that member was an unresolvable
 * `MSCP$DISK` connect; its `recv_ack` stayed 0 for the whole 1620-second run
 * and the join never advanced past its own directory round.
 *
 * ===========================================================================
 * WHAT THIS FILE REFUSES TO INVENT (INV-6)
 *
 * SILENCE IS NOT AN ANSWER. `MSCP_CL_CONN_ABSENT` is reached ONLY from the
 * wire's own literal "NOT PRESENT HERE" (vms_scs_dir.h delivers that as
 * `present == 0`). An inquiry nobody answered is NOT recorded as absence: it
 * times out back to `IDLE`, is counted in `unanswered`, and is asked again --
 * which is the recovery p. 2-51 itself names ("Periodically, the Process
 * Poller ... connects to the Directory Service"). Recording silence as "this
 * member serves no disks" would fabricate an answer a member never gave.
 *
 * THE MEMBERSHIP READ IS THE EXECUTIVE'S. `ops->joined` is a read of
 * `struct vms_cluster.state` in production -- never a flag this FSM sets when
 * it feels ready.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 * PURE TU: no seam primitive, no allocation, no clock but ops->now_ms, so a
 * host test drives a thirty-second retry window in microseconds.
 */
#ifndef OVMX_VMS_MSCP_CL_CONN_FSM_H
#define OVMX_VMS_MSCP_CL_CONN_FSM_H

#include "vms_cluster.h"
#include "vms_scs.h"

/* ==========================================================================
 * 1. Cadence -- OVMX design values, each labelled as one
 *
 * No capture measures a class-driver retry interval; what IS grounded is the
 * shape (the reference joiner's reciprocal disk-client half follows its join
 * by ~1 s, and a member that serves nothing keeps refusing). So these bound
 * how often this node ASKS, never what it concludes.
 * ========================================================================== */

/* How long an unanswered directory inquiry is waited on before it is counted
 * and asked again. The reference exchanges answer in single-digit
 * milliseconds; this is a backstop, not a schedule. */
#define MSCP_CL_CONN_ASK_TIMEOUT_MS 5000u

/* How long after a refusal, a close, or a "NOT PRESENT HERE" this node waits
 * before asking that member again. A member with MSCP_LOAD=0 must not be
 * asked once a second; a member that mounts its first volume later must still
 * be found. */
#define MSCP_CL_CONN_RETRY_MS 30000u

/* ==========================================================================
 * 2. The states -- one per position a member's disk-client leg can occupy
 * ========================================================================== */
enum mscp_cl_conn_state {
	MSCP_CL_CONN_IDLE       = 0, /* nothing asked, or an answer aged out */
	MSCP_CL_CONN_ASKING     = 1, /* our directory inquiry is outstanding */
	MSCP_CL_CONN_PRESENT    = 2, /* it really answered YES; connect due  */
	MSCP_CL_CONN_ABSENT     = 3, /* it really answered NOT PRESENT HERE  */
	MSCP_CL_CONN_CONNECTING = 4, /* our connect went out; not open yet   */
	MSCP_CL_CONN_OPEN       = 5, /* SCS says the connection is OPEN      */
	MSCP_CL_CONN_STATE__COUNT
};

/* ==========================================================================
 * 3. The events -- one per FACT, never a composed one
 * ========================================================================== */
enum mscp_cl_conn_event {
	MSCP_CL_CONN_EV_SWEEP  = 0, /* the beat reached this member          */
	MSCP_CL_CONN_EV_HIT    = 1, /* it answered: yes, I host MSCP$DISK    */
	MSCP_CL_CONN_EV_MISS   = 2, /* it answered: NOT PRESENT HERE         */
	MSCP_CL_CONN_EV_OPENED = 3, /* SCS says our connection reached OPEN  */
	MSCP_CL_CONN_EV_CLOSED = 4, /* it closed, or was refused             */
	MSCP_CL_CONN_EV__COUNT
};

const char *mscp_cl_conn_state_name(enum mscp_cl_conn_state s);

/* ==========================================================================
 * 4. The injected ops -- this FSM's ONLY route to the world
 * ========================================================================== */
struct mscp_cl_conn_ops {
	/*
	 * Is this node PAST its own join drive -- i.e. a cluster member?
	 * Production: `cl->state == VMS_CLUSTER_MEMBER`, a read of the state
	 * the connection manager really reached. Nonzero = yes. A NULL op is
	 * read as "no", which stops the sweep rather than guessing.
	 */
	int (*joined)(void *ctx);

	/*
	 * Does the connection manager's OWN join already hold the MSCP$DISK
	 * connection to `dst`? Production: cnxman_join_holds_disk_client,
	 * which reads the join FSM's real `mscp_conid`/`target_sysid`. The
	 * reference joiner presents exactly ONE VMS$DISK_CL_DRVR -> MSCP$DISK
	 * connection per member (2->3 reference: one to each of VAX1 and
	 * VAX2), so this sweep must not open a second one behind the join's.
	 * A NULL op is read as "no", which is the safe direction for a wiring
	 * that has no join.
	 *
	 * (The join's connection is not HANDED OVER to this driver today --
	 * that is a layering question beyond E64 -- so this node simply does
	 * not double-connect, and `deferred_join_owned` counts every time it
	 * declined to.)
	 */
	int (*join_holds)(void *ctx, vms_scs_sysid_t dst);

	/*
	 * Ask `dst` whether it hosts `name`. Production: scs_dir_lookup over
	 * the transient SCS$DIRECTORY round of vms_scs_dir.h. 0 = the inquiry
	 * went out. The answer comes back as EV_HIT / EV_MISS; silence comes
	 * back as nothing at all.
	 */
	int (*dir_inquire)(void *ctx, vms_scs_sysid_t dst,
			   const uint8_t *name);

	/*
	 * Open VMS$DISK_CL_DRVR -> MSCP$DISK on `dst`. Production:
	 * cnxman_disk_client_connect, CNXMAN's ONE VMS$DISK_CL_DRVR
	 * registration. 0 and a nonzero `*out_conid` = it went out.
	 */
	int (*connect)(void *ctx, vms_scs_sysid_t dst, vms_conid_t *out_conid);

	uint32_t (*now_ms)(void *ctx);
	void     (*log)(void *ctx, const char *msg);

	void *ctx;
};

/* ==========================================================================
 * 5. The objects (the caller owns the table; this module allocates nothing)
 * ========================================================================== */

/* One member's disk-client leg. Every field is a fact this node established:
 * the sysid SCS reported, the Con.ID the allocator minted for OUR connect,
 * and when we last spoke to it. */
struct mscp_cl_conn_peer {
	uint8_t         in_use;
	uint8_t         state;       /* enum mscp_cl_conn_state              */
	uint8_t         spoken;      /* we have asked this member at least
				      * once -- so a NEWLY discovered member
				      * is asked on the FIRST sweep that sees
				      * it and only a RE-ask waits out the
				      * retry period (the reference joiner's
				      * reciprocal half starts one beat after
				      * its own join, not thirty seconds)    */
	uint8_t         pad0;
	vms_scs_sysid_t sysid;
	vms_conid_t     conid;       /* 0 unless CONNECTING or OPEN          */
	uint32_t        since_ms;    /* when this state was entered          */
};

struct mscp_cl_conn {
	const struct mscp_cl_conn_ops *ops;

	struct mscp_cl_conn_peer *peers;
	uint32_t                  n_peers;

	/* The 16-byte blank-padded name this FSM asks about and connects to.
	 * Bound by the caller from the ONE spelling in vms_cnxman_join_fsm.c;
	 * this file re-types no SYSAP name. */
	const uint8_t *name_mscp_disk;

	/* ---- counters, every one a real dispatch of a real event ---- */
	uint32_t sweeps;             /* beats delivered                      */
	uint32_t deferred_joining;   /* peer-sweeps skipped: still joining   */
	uint32_t deferred_join_owned;/* ... skipped: the join holds that leg  */
	uint32_t inquiries;          /* directory inquiries really sent      */
	uint32_t inquiry_failures;   /* ... that the directory would not take*/
	uint32_t hits;               /* members that answered YES            */
	uint32_t misses;             /* ... and NOT PRESENT HERE             */
	uint32_t unanswered;         /* inquiries nobody ever answered       */
	uint32_t connects;           /* MSCP$DISK connections opened         */
	uint32_t connect_refusals;   /* ... that SCS/CNXMAN refused          */
	uint32_t opens;              /* connections that reached OPEN        */
	uint32_t closes;             /* ... and later went away              */
	uint32_t no_peer_slot;       /* a member the table had no room for   */
	uint32_t ignored_events;     /* [state][event] with no edge: COUNTED */
};

/* ==========================================================================
 * 6. Lifecycle and binding
 * ========================================================================== */

/* Zero the context and bind the ops and the one SYSAP name. Sends nothing. */
int mscp_cl_conn_init(struct mscp_cl_conn *c,
		      const struct mscp_cl_conn_ops *ops,
		      const uint8_t *name_mscp_disk);

int mscp_cl_conn_bind_peers(struct mscp_cl_conn *c,
			    struct mscp_cl_conn_peer *p, uint32_t n);

/* ==========================================================================
 * 7. Events -- one entry point per FACT
 * ========================================================================== */

/*
 * One beat: `sysids[0..n)` are the systems the PORT genuinely has a circuit
 * to (production: the vms_scs_peer_at sweep). Members that have gone are
 * dropped; new ones get a row. Returns the number of peers actually swept --
 * 0 while this node is still joining, which is Rule 2 doing its job.
 */
uint32_t mscp_cl_conn_sweep(struct mscp_cl_conn *c,
			    const vms_scs_sysid_t *sysids, uint32_t n);

/* `dst` answered our inquiry about MSCP$DISK. `present` nonzero is the HIT;
 * zero is the wire's literal "NOT PRESENT HERE". There is no third value, and
 * an unanswered inquiry never arrives here at all. */
void mscp_cl_conn_dir_result(struct mscp_cl_conn *c, vms_scs_sysid_t dst,
			     const uint8_t *name, int present);

/* Our connection to a member reached OPEN / went away. */
void mscp_cl_conn_opened(struct mscp_cl_conn *c, vms_conid_t conid);
void mscp_cl_conn_closed(struct mscp_cl_conn *c, vms_conid_t conid);

/* Readback: the row for `sysid` (or for `conid`), or NULL. */
struct mscp_cl_conn_peer *mscp_cl_conn_by_sysid(struct mscp_cl_conn *c,
						vms_scs_sysid_t sysid);
struct mscp_cl_conn_peer *mscp_cl_conn_by_conid(struct mscp_cl_conn *c,
						vms_conid_t conid);

#endif /* OVMX_VMS_MSCP_CL_CONN_FSM_H */
