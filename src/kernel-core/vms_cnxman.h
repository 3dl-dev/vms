/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman.h - the Connection Manager: CLUB/CSB query, the transition
 * callbacks into the DLM, and cluster-event delivery (FC-P0.1).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.2 (what vms_cnxman.c
 * owns), SS3.4 (CLUB/CSB and the LEARNED local CSID), SS3.5 (the personality
 * readback), SS3.7 (quorum and the coordinator). Wire spec:
 * docs/cluster-protocol-spec.md SS4(j)/(L)/(o)/(p)/(q)/(r)/(y)/(aa).
 *
 * THE LAYER IN ONE PARAGRAPH. CNXMAN decides who the cluster IS. It keeps the
 * CLUB (this node's cluster block: the local CSID, the epoch, the membership
 * bitmap, votes and quorum, the transition in progress) and one CSB per remote
 * connection manager. It drives the join, participates in the 12-step
 * transition barrier, relays and -- when the documented condition holds and no
 * other CM has opened a transition -- coordinates one, runs the
 * RECNXINTERVAL/TIMVCFAIL reconnect loop, and emits the %CNXMAN console lines.
 * SHOW CLUSTER and $GETSYI read the CSBs it holds; they do not read a mirror.
 *
 * THE LOCAL CSID IS LEARNED, NEVER CHOSEN. The cluster assigns it during the
 * ADD transition and CNXMAN finds its own by matching its SCSSYSTEMID in the
 * membership records. Until it is learned this node is NEW and issues no DLM
 * traffic. The predecessor -- an insmod parameter defaulting to 1 -- made the
 * executive a phantom cluster of one, which is why no accessor here returns a
 * CSID without a validity flag beside it.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_CNXMAN_H
#define OVMX_VMS_CNXMAN_H

#include "vms_cluster.h"
#include "vms_cluster_snapshot.h"
#include "vms_dlm_scs.h"   /* struct dlm_scs_role_ops -- the transition callbacks */

struct vms_cnxman;

/* ==========================================================================
 * 1. A transition
 *
 * The unit of membership change: a class, an epoch, a coordinator, and a
 * position in the 12-step barrier. Passed to the DLM's transition callbacks by
 * const pointer -- the DLM reads it, never mutates it, and never keeps the
 * pointer past the call (VAX kernel stacks are small; nothing here is copied
 * onto one).
 * ========================================================================== */
struct cnxman_transition {
	uint32_t epoch;              /* the transition epoch as the wire carried it */
	uint8_t  tr_class;           /* enum vms_cnxman_transition_class */
	uint8_t  barrier_step;       /* 0..12 */
	uint8_t  coordinator_valid;  /* 0 = no coordinator identified yet */
	uint8_t  we_coordinate;      /* nonzero iff this node drives the barrier */
	vms_csid_t coordinator_csid;
	vms_csid_t subject_csid;     /* the node being added/removed/departing ... */
	uint8_t  subject_csid_valid; /* ... 0 while it is still unidentified */
	uint8_t  pad[3];
};

/* ==========================================================================
 * 2. Timers and injected ops
 * ========================================================================== */
enum cnxman_timer {
	CNXMAN_TIMER_RECNX     = 0,  /* the once-per-second RECNXINTERVAL loop */
	CNXMAN_TIMER_JOIN      = 1,  /* a join step awaiting its answer */
	CNXMAN_TIMER_BARRIER   = 2,  /* barrier-step watchdog: INSTRUMENT ONLY --
				      * the spec says do not time a slow step out */
	/*
	 * The coordinator's collision back-off (FC-P3.12). Book p. 7-32: a
	 * connection manager that cannot get the coordinator lock because
	 * another already holds it "backs off a random short interval" and
	 * re-evaluates. It is a decision timer, not a protocol timeout: nothing
	 * on the wire expires when it fires.
	 */
	CNXMAN_TIMER_COORD     = 3,
	CNXMAN_TIMER__COUNT
};

struct cnxman_ops {
	/* Send one CM message on the VMS$VAXcluster connection to `dst`.
	 * Production: scs_send_msg on the CSB's CDT. */
	int  (*send)(void *ctx, vms_csid_t dst, const uint8_t *body, uint32_t len);

	/* Answer the request currently being dispatched, on its own connection,
	 * correlated with its transaction. Distinct from `send` because a
	 * RESPONSE and an ORIGINATION are different acts: a response the peer
	 * cannot correlate is rejected as ungrounded. */
	int  (*respond)(void *ctx, const uint8_t *body, uint32_t len);

	void (*arm_timer)(void *ctx, enum cnxman_timer which, uint32_t key, uint32_t ms);
	void (*cancel_timer)(void *ctx, enum cnxman_timer which, uint32_t key);

	uint32_t (*now_ms)(void *ctx);
	void     (*log)(void *ctx, const char *msg);   /* the %CNXMAN lines */
	void    *(*alloc)(void *ctx, uint32_t n);
	void     (*free)(void *ctx, void *p);

	void *ctx;
};

/* ==========================================================================
 * 3. The event vocabulary
 *
 * The connection manager is several tables, not one (FC-P3.3 join, FC-P3.5
 * barrier participant, FC-P3.12 coordinator, FC-P3.6 reconnect), all indexed by
 * this shared event enum so one recorded transcript reads across them.
 * ========================================================================== */
enum cnxman_event {
	/* Connectivity, from SCS. */
	CNXMAN_EV_CDT_OPEN      = 0,   /* a VMS$VAXcluster connection came up */
	CNXMAN_EV_CDT_CLOSED    = 1,

	/* Membership and transition messages (categories/opcodes are the codec's
	 * business; the FSM sees the MEANING, so an opcode re-assignment after a
	 * capture is a table edit and not a redesign). */
	CNXMAN_EV_RX_MEMBERSHIP = 2,   /* a membership/status burst */
	CNXMAN_EV_RX_TR_REQUEST = 3,   /* someone asks for a transition */
	CNXMAN_EV_RX_TR_RELAY   = 4,   /* the coordinator relays it */
	CNXMAN_EV_RX_TR_OPEN    = 5,   /* the transition opens (epoch + bitmap) */
	CNXMAN_EV_RX_TR_GO      = 6,   /* GO: the barrier begins */
	CNXMAN_EV_RX_BARRIER    = 7,   /* one barrier step */
	CNXMAN_EV_RX_BARRIER_ACK = 8,
	CNXMAN_EV_RX_REBUILD    = 9,   /* a lock-rebuild record: to the DLM */
	CNXMAN_EV_RX_CLOSE      = 10,  /* the transition closes */

	/* Local facts. */
	CNXMAN_EV_START         = 11,  /* CLUSTER_START: begin forming or joining */
	CNXMAN_EV_CSID_LEARNED  = 12,  /* our SCSSYSTEMID appeared in the records */
	CNXMAN_EV_TIMER_RECNX   = 13,
	CNXMAN_EV_TIMER_JOIN    = 14,
	CNXMAN_EV_TIMER_BARRIER = 15,
	CNXMAN_EV_SHUTDOWN      = 16,  /* orderly stop; emits the last gasp */

	/*
	 * A RESPONSE to a request THIS node originated (FC-P3.12). Added rather
	 * than folded into one of the request events above, because a request
	 * and its answer are different facts: an inbound op-0x12 is another
	 * connection manager relaying ITS transition at us (a collision), while
	 * a 0x81/0x12 is a member confirming connectivity for OURS (the commit
	 * gate, wire spec SS4(O.31)). Stretching one cell to carry both would
	 * misname the collision in every transcript.
	 *
	 * The coordinator drives a strict sequence and has exactly ONE class of
	 * request outstanding per state (relay -> commit -> Phase 1 open ->
	 * barrier), so the [state] half of its table is what says WHICH answer
	 * this is; the handler then verifies the opcode really is the one that
	 * state is waiting on and COUNTS it as out-of-order otherwise.
	 */
	CNXMAN_EV_RX_TR_ACK     = 17,

	/*
	 * THE JOIN'S OWN FACTS (FC-P3.3). Added rather than folded into the
	 * cells above for the same reason CNXMAN_EV_RX_TR_ACK was: a join is a
	 * CLIENT run against the cluster's other SYSAPs (spec SS4(L): the
	 * joiner opens its own SCS$DIRECTORY connection, looks a name up,
	 * opens MSCP$DISK, walks the served units, and only then opens the
	 * VMS$VAXcluster VC), and none of those facts is a membership message.
	 * Folding them into CNXMAN_EV_RX_MEMBERSHIP would make every recorded
	 * transcript say "membership burst" where the wire said "the member
	 * answered a directory lookup".
	 *
	 * WHICH connection a CDT_OPEN/CDT_CLOSED refers to is answered by the
	 * [state] half of the join table plus the Con.ID the FSM recorded when
	 * it issued that connect -- exactly the discipline the coordinator's
	 * one-outstanding-request-per-state note above describes.
	 */
	CNXMAN_EV_DIR_RESULT    = 18,  /* an SCS$DIRECTORY inquiry was ANSWERED
					* (a real yes/no from the peer; an
					* unanswered one never arrives here --
					* vms_scs_dir.h reports silence as
					* silence, never as absence) */
	CNXMAN_EV_MSCP_END      = 19,  /* an MSCP END arrived on MSCP$DISK */
	CNXMAN_EV_RX_CONFIG     = 20,  /* the peer's own cat-0x01 op-0x14 /
					* op-0x01 / op-0x02 advertisement --
					* spec SS4(o) row 3, "the peer
					* reciprocates in kind" */
	CNXMAN_EV_RX_COMMIT     = 21,  /* cat-0x01 op-0x03 membership COMMIT or
					* op-0x05 lock/resource rebuild: the
					* member-driven, (txn,token)-correlated
					* requests the joiner echoes (SS4(o)
					* rows 5-9) */

	CNXMAN_EV__COUNT
};

/* ==========================================================================
 * 4. Cluster events delivered to the personality ($SETCLUEVT)
 *
 * The two event types $CLUEVTDEF publishes. More are added only when a real
 * event is grounded; an event enum is a promise that the executive can detect
 * the thing it names.
 * ========================================================================== */
enum cnxman_cluster_event {
	CNXMAN_CLUEVT_ADD    = 0,   /* a node joined */
	CNXMAN_CLUEVT_REMOVE = 1,   /* a node left */
	CNXMAN_CLUEVT__COUNT
};

/* ==========================================================================
 * 5. The interface to the DLM
 *
 * Install the lock manager's wire arm. CNXMAN then calls it at every transition
 * boundary and hands it every cat-02 message that arrives on the
 * VMS$VAXcluster connection -- WITH a reply buffer, so the answer goes back
 * through the correlated response path and nowhere else (vms_dlm_scs.h RULE A).
 * Passing NULL detaches it: the cluster still forms and membership still works
 * with no distributed locking, which is a real VMS configuration and an honest
 * degradation, not a simulation.
 * ========================================================================== */
void cnxman_set_dlm(struct vms_cluster *cl, const struct dlm_scs_role_ops *ops);

/* ==========================================================================
 * 5b. The interface to the DISK CLASS DRIVER (FC-P7.1)
 *
 * WHY IT HANGS OFF CNXMAN AT ALL. `VMS$DISK_CL_DRVR` is ONE SYSAP name and SCS
 * allows ONE registration per name (vms_scs.h SS4). CNXMAN already registers it
 * -- its own comment says "ONLY so scs_connect() ... can open the outbound
 * MSCP$DISK client connection the join drives" -- so the disk class driver
 * cannot register it a second time, and giving the class driver a DIFFERENT
 * local name would put an invented SYSAP name on the wire where every real VMS
 * class driver puts `VMS$DISK_CL_DRVR`.
 *
 * So there is exactly one registration and TWO consumers of it, and CNXMAN
 * fans each callback out to both. Neither guesses: the join FSM already
 * ignores a Con.ID that is not the one it opened, and the class driver ignores
 * a Con.ID for which it holds no CDDB. A connection belongs to whichever of
 * them opened it, which is a fact each of them holds, not an inference.
 *
 * The class driver opens its OWN connections with scs_connect() under this same
 * registered name -- SCS requires the name to be registered, not that the
 * registrant be the caller -- and learns their fate through these three.
 * Passing NULL detaches it: the cluster still forms and the join's own walk
 * still runs, which is what a node with no served disks to mount looks like.
 * ========================================================================== */
struct cnxman_disk_client_ops {
	/* One of the class driver's own MSCP$DISK connections reached OPEN. */
	void (*opened)(void *ctx, vms_conid_t local_conid);
	/* An MSCP end message arrived on some MSCP$DISK connection. Returns 0
	 * when this consumer took it. */
	int  (*message)(void *ctx, vms_conid_t local_conid, const uint8_t *body,
			uint32_t len);
	void (*closed)(void *ctx, vms_conid_t local_conid, uint32_t reason);
	void *ctx;
};

void cnxman_set_disk_client(struct vms_cluster *cl,
			    const struct cnxman_disk_client_ops *ops);

/*
 * Open an outbound `VMS$DISK_CL_DRVR` -> `MSCP$DISK` connection to `dst` under
 * CNXMAN's registration (see above). *out_conid is the Con.ID the ALLOCATOR
 * minted. Returns 0 or an SS$_ status -- SS$_NOSUCHDEV before CLUSTER_START.
 * This is the ONE door: the class driver never names the two SYSAP name
 * literals itself, because they live in vms_cnxman_join_fsm.c and there is one
 * spelling of each in the tree.
 */
int cnxman_disk_client_connect(struct vms_cluster *cl, vms_scs_sysid_t dst,
			       vms_conid_t *out_conid);

/* ==========================================================================
 * 6. CLUB / CSB query -- what SHOW CLUSTER, $GETSYI and the diagnostics read
 *
 * All four are read-only projections taken under the fork mutex. They return 0
 * when the cluster stack is running (even if this node is not a member -- that
 * is what the view's flags say) and SS$_NOSUCHDEV when it is not started at all.
 * A caller NEVER infers membership from a zero CSID: it reads
 * `local_csid_valid`.
 * ========================================================================== */
int cnxman_get_club(struct vms_cluster *cl, struct vms_club_view *out);

/* Walk the CSB table by index; SS$_NOSUCHDEV past the end. */
int cnxman_get_csb(struct vms_cluster *cl, uint32_t index,
		   struct vms_csb_view *out);

/* Find a CSB by the CSID the cluster assigned; nonzero if there is none. */
int cnxman_find_csb(struct vms_cluster *cl, vms_csid_t csid,
		    struct vms_csb_view *out);

/* The current transition, if one is in progress. Returns 0 and fills `out` when
 * there is one; nonzero when there is not (NOT a zeroed struct that reads like
 * a transition at epoch 0). */
int cnxman_get_transition(struct vms_cluster *cl, struct cnxman_transition *out);

/* ==========================================================================
 * 7. Lifecycle (glue, vms_cnxman.c -- FC-P3.8/FC-P3.9)
 * ========================================================================== */

/*
 * Form or join. Reproduces SYSINIT's ordering: STARTUP.EXE calls this through
 * VMS_IOCTL_CLUSTER_START before the system disk is mounted.
 *   VAXCLUSTER=0  -> returns immediately, state VMS_CLUSTER_OFF
 *   VAXCLUSTER=1  -> joins if a cluster is present, else STANDALONE
 *   VAXCLUSTER=2  -> waits, printing VMS's own
 *                    "waiting to form or join an OpenVMS Cluster" on OPA0:
 * Returns SS$_NORMAL with cl->state set, or an SS$_ status if a layer beneath
 * (fork/port/SCS) is not running.
 *
 * "PRESENT" AND "WAITS", CONCRETELY (FC-P3.9). Presence is asked of the
 * interconnect: the systems SCS has an OPEN circuit to (vms_scs_peer_at(),
 * vms_scs.h SS8) get a CSB, and the join is driven through one of them. The
 * VAXCLUSTER=2 wait is NON-BLOCKING -- this call returns, the state stays
 * JOINING, and the reconnect beat re-sweeps for peers, so a member that boots
 * later is still joined. Neither path can return MEMBER: only a real
 * membership record naming this node's SCSSYSTEMID sets that.
 *
 * IDEMPOTENT. A second call while the connection manager is up is
 * SS$_NORMAL and starts nothing -- it does not re-drive a join in flight.
 */
int vms_cnxman_start(struct vms_cluster *cl);

/* Leave the cluster: emit the last gasp, close the connections, stop the
 * timers. Idempotent. */
void vms_cnxman_stop(struct vms_cluster *cl);

/* ==========================================================================
 * 8. $SETCLUEVT (glue, vms_cnxman.c -- FC-P3.8)
 *
 * `proc` is an opaque handle (this header names no substrate/process type,
 * design SS3.2.2): the caller is vms_devtab.c's ioctl handler, which holds a
 * real `struct vms_proc *` and passes it through unexamined. A single
 * registration per node -- the caller wanting more is a design question for
 * whoever asks, not something this glue invents a table for.
 * ========================================================================== */

/* Register (or, with event_mask 0 or astadr 0, clear) this node's ONE
 * $SETCLUEVT registration: `event_mask` is CLUEVT$C_ADD/_REMOVE bits
 * (cluevtdef.h), delivered as a completion AST to `proc` at `astadr` with
 * `astprm`. SS$_NOSUCHDEV before CLUSTER_START. */
int vms_cnxman_cluevt_set(struct vms_cluster *cl, void *proc,
			  uint32_t event_mask, uint64_t astadr,
			  uint64_t astprm);

/* Called at process death (see this item's report for the two call sites):
 * clears the registration if it belongs to `proc`, so a delivery can never
 * reach freed memory. A no-op if `proc` never registered. */
void vms_cnxman_proc_gone(struct vms_cluster *cl, void *proc);

#endif /* OVMX_VMS_CNXMAN_H */
