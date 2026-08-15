/*
 * scs_recnx.h - the Connection Manager CSB connectivity state machine and the
 * RECNXINTERVAL virtual-circuit-breakage reconnect loop (vms-c7d).
 *
 * WHY THIS EXISTS. When a virtual circuit between the local SYS$CLUSTER and a
 * remote SYS$CLUSTER breaks for any reason OTHER than a "last gasp" datagram,
 * VMS does NOT immediately expel the remote node from the cluster. The local
 * Connection Manager (module CNXMAN) holds the node's membership and tries,
 * once a second, to re-establish the connection for a bounded period. Only if
 * every reconnect attempt in that period fails -- and no other Connection
 * Manager has already started a cluster state transition -- does the local
 * Connection Manager start a transition to reconfigure the cluster.
 *
 * OVMX previously had no such layer: a broken circuit dropped straight through
 * scs_vc_break() to CLOSED with nothing holding the node in the cluster and no
 * timed reconnect. This module is the CM-layer model of the two documented
 * mechanisms:
 *
 *   1. THE CSB CONNECTIVITY STATE MACHINE. Each remote Connection Manager is
 *      represented locally by a Cluster System Block (CSB) whose "connectivity"
 *      field tracks the state of the SCS connection to that remote CM. VMS names
 *      TEN such states (transcript ch7-part02 pp. 7-23/7-24): NEW, CONNECT,
 *      ACCEPT, OPEN, DISCONNECT, WAIT, RECONNECT, REACCEPT, DEAD, LOCAL.
 *
 *   2. THE RECNXINTERVAL RECONNECT LOOP (transcript pp. 7-30). "For a limited
 *      period of time, the local Connection Manager will attempt once a second
 *      to establish another connection with the remote Connection Manager. If
 *      all such reconnect attempts fail, and if no other Connection Manager has
 *      already instituted a cluster state transition, then the local Connection
 *      Manager starts a state transition to reconfigure the cluster." The
 *      limited period is "the maximum of the local value for RECNXINTERVAL and a
 *      port dependent number supplied by the remote Connection Manager"; for a
 *      LAN virtual circuit that remote value was fixed at 16 prior to V5.5 and,
 *      from V5.5, is the remote system's TIMVCFAIL parameter.
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md rule 8). The state list and the reconnect
 * rule above are transcribed from the mined VAXcluster internals transcript
 * (host-only) and the public "VMScluster Systems for OpenVMS" manual. No VSI or
 * HPE source or binary was read. Where VMS publishes only the RULE and not a
 * numeric constant (the retry cadence is stated as "once a second"; the period
 * is a max() of two named SYSGEN parameters), OVMX uses exactly those documented
 * quantities and treats RECNXINTERVAL / the remote port value as caller-supplied
 * configuration -- it invents no VMS number. The 16 pre-V5.5 LAN constant and
 * the once-a-second cadence ARE published and are used verbatim.
 *
 * DETERMINISM. Every time value is a caller-supplied monotonic-millisecond
 * stamp (as in scs_vc.c's retransmit engine and scsd.c's join_retx_tick), so the
 * whole reconnect loop is a pure function of (state, clock) and is unit-testable
 * by injecting a virtual-circuit breakage and then driving the clock -- no wall
 * clock and no socket inside the logic.
 */
#ifndef SCS_RECNX_H
#define SCS_RECNX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The ten CSB "connectivity" states (transcript pp. 7-23/7-24), in the order the
 * book lists them. The names are VMS's own. The task brief named this machine's
 * loss/retry states "TIMEOUT / RECONNECT / RECONNECTING"; those map onto the
 * book's WAIT (the timeout is in progress) and RECONNECT (a reconnect attempt is
 * in progress) respectively -- the source-of-truth hierarchy puts the reference
 * transcript above the brief, so the book's names are what this enum carries.
 */
enum scs_csb_conn_state {
    SCS_CSB_NEW = 0,   /* CSB just allocated: a newly discovered, or returning, remote CM */
    SCS_CSB_CONNECT,   /* the initial SCS CONNECT request has been sent to it */
    SCS_CSB_ACCEPT,    /* an initial SCS CONNECT request from it is being accepted */
    SCS_CSB_OPEN,      /* an SCS connection exists -- the normal state of a CSB */
    SCS_CSB_DISCONNECT,/* an SCS DISCONNECT is in progress for the open connection */
    SCS_CSB_WAIT,      /* connectivity lost; a timeout is in progress before reconnect (p. 7-24) */
    SCS_CSB_RECONNECT, /* a reconnect attempt to the remote CM is in progress */
    SCS_CSB_REACCEPT,  /* the local CM is accepting a reconnect request from the remote CM */
    SCS_CSB_DEAD,      /* a new incarnation of the VAX has been seen; this CSB is the old one */
    SCS_CSB_LOCAL      /* the special CSB representing the local Connection Manager */
};

/* What the reconnect loop asks its caller (scsd.c's CM) to do on a tick. */
enum scs_recnx_action {
    SCS_RECNX_ACT_NONE = 0,          /* nothing to do this tick (still waiting) */
    SCS_RECNX_ACT_RECONNECT,         /* issue one reconnect attempt now (the once-a-second beat) */
    SCS_RECNX_ACT_PROPOSE_TRANSITION /* period expired, no peer transition -> start a state transition */
};

/*
 * The once-a-second reconnect cadence (transcript p. 7-30: "attempt once a
 * second"). PUBLISHED, not an OVMX choice.
 */
#define SCS_RECNX_ATTEMPT_INTERVAL_MS 1000u

/*
 * The pre-V5.5 fixed LAN remote value (transcript p. 7-30: "was fixed at 16
 * prior to Version 5.5"). PUBLISHED, not an OVMX choice. Seconds.
 */
#define SCS_RECNX_LAN_PRE_V55_VALUE 16u

/*
 * An OVMX default for the local RECNXINTERVAL when the daemon is given none. The
 * reference lab runs SYSGEN RECNXINTERVAL = 20 (docs/cluster-protocol-spec.md
 * sec references the lab's value 20); OVMX uses that as its fallback but the
 * value is configuration, NOT a claimed VMS invariant -- callers pass their own.
 *
 * vms-c3b: RECNXINTERVAL is now a first-class AUTHORED SYSGEN parameter
 * (tools/vms_sysgen.c default_params[], default 20, GROUNDED from the public
 * OpenVMS System Management Utilities Reference Manual). scsd_recnxinterval()
 * adopts the authored value from SYS$SYSTEM:OVMXVMSSYS.PAR and falls back to
 * THIS constant, so the two agree by construction -- keep them in step (both 20)
 * if either is ever re-grounded.
 */
#define SCS_RECNX_DEFAULT_RECNXINTERVAL 20u

/*
 * Environment KILL SWITCH (guardrail 23). OVMX_NO_RECNX_RECONNECT set to
 * anything other than "0" reverts scs_csb_connectivity_lost() to the pre-vms-c7d
 * behaviour: a non-last-gasp VC breakage drops the node's membership IMMEDIATELY
 * and proposes a transition, with no WAIT state and no timed reconnect. With the
 * switch unset (or "0"), the documented p. 7-30 behaviour applies: enter WAIT,
 * HOLD membership, and let the reconnect tick retry once a second. Read fresh on
 * every call so a test can bracket a single call.
 */
#define SCS_RECNX_NO_RECONNECT_ENV "OVMX_NO_RECNX_RECONNECT"

/*
 * struct scs_csb - the connectivity-relevant slice of a Cluster System Block.
 *
 * This is NOT the whole CSB (which also holds VOTES/EXPECTED_VOTES/QDSKVOTES for
 * the quorum algorithm -- see scs_quorum.c -- and the LOCKDIRWT weight). It is
 * the connection-state field the transcript pp. 7-23/7-24 describe plus the
 * RECNXINTERVAL reconnect bookkeeping p. 7-30 requires.
 */
struct scs_csb {
    enum scs_csb_conn_state state;
    int is_local;                 /* 1 = the LOCAL CSB (never reconnected/expelled) */
    int member;                   /* 1 = this node is currently held as a cluster member */

    /* RECNXINTERVAL reconnect period inputs (transcript p. 7-30), in SECONDS as
     * VMS states them. The effective period is max(local, remote). */
    unsigned local_recnxinterval; /* the local SYSGEN RECNXINTERVAL */
    unsigned remote_port_value;   /* the port-dependent value the remote CM supplied */

    /* Reconnect-loop clock state (caller-supplied monotonic ms). */
    uint64_t lost_ms;             /* when connectivity was lost = when WAIT was entered */
    uint64_t last_attempt_ms;     /* monotonic ms of the last reconnect attempt (or of WAIT entry) */
    unsigned attempts;            /* reconnect attempts made since WAIT was entered */

    /* Observability. */
    unsigned long reconnects;             /* successful reconnections */
    unsigned long transitions_proposed;   /* state transitions this CSB started */
};

/*
 * scs_csb_init - initialise a CSB. is_local != 0 gives the LOCAL CSB (state
 * LOCAL, never a reconnect subject); otherwise a freshly discovered remote CM
 * (state NEW). local_recnxinterval seeds the reconnect period; 0 is replaced by
 * SCS_RECNX_DEFAULT_RECNXINTERVAL. No-op if csb is NULL.
 */
void scs_csb_init(struct scs_csb *csb, int is_local, unsigned local_recnxinterval);

/*
 * scs_csb_set_remote_port_value - record the port-dependent reconnect value the
 * remote Connection Manager supplied (seconds), used as the second operand of
 * the max() that sizes the reconnect period. No-op if csb is NULL.
 */
void scs_csb_set_remote_port_value(struct scs_csb *csb, unsigned secs);

/*
 * scs_recnx_lan_remote_value - the LAN remote reconnect value (transcript
 * p. 7-30): SCS_RECNX_LAN_PRE_V55_VALUE (16) when v55_or_later == 0, else the
 * remote system's TIMVCFAIL. Seconds.
 */
unsigned scs_recnx_lan_remote_value(int v55_or_later, unsigned remote_timvcfail);

/*
 * scs_recnx_timeout_secs - the reconnect period (transcript p. 7-30): "the
 * maximum of the local value for RECNXINTERVAL and a port dependent number
 * supplied by the remote Connection Manager." Seconds. The book's worked example
 * (local 20, remote 10) yields 20.
 */
unsigned scs_recnx_timeout_secs(unsigned local_recnxinterval, unsigned remote_port_value);

/*
 * scs_csb_reconnect_enabled - 0 iff SCS_RECNX_NO_RECONNECT_ENV is set to a value
 * other than "0" (the kill switch: pre-vms-c7d immediate-drop behaviour). 1
 * otherwise. Read fresh on every call.
 */
int scs_csb_reconnect_enabled(void);

/*
 * scs_csb_connectivity_gained - the SCS connection to this remote CM is (re)open:
 * OPEN, membership held, reconnect bookkeeping cleared. If the CSB was in WAIT,
 * RECONNECT, or REACCEPT this counts as a successful reconnection (bumps
 * reconnects). No-op if csb is NULL or is_local.
 */
void scs_csb_connectivity_gained(struct scs_csb *csb, uint64_t now_ms);

/*
 * scs_csb_connectivity_lost - the virtual circuit to this remote CM has broken.
 *
 *   last_gasp != 0: a "last gasp" bugcheck/shutdown datagram (transcript
 *     p. 7-29). The circuit is closed and a transition follows immediately;
 *     membership is dropped and SCS_RECNX_ACT_PROPOSE_TRANSITION is returned. NO
 *     reconnect wait -- the remote node has announced it is leaving.
 *
 *   last_gasp == 0, kill switch armed: the pre-vms-c7d behaviour -- drop
 *     membership immediately and PROPOSE_TRANSITION, no WAIT, no reconnect.
 *
 *   last_gasp == 0, normal: enter WAIT at now_ms, HOLD membership, and return
 *     SCS_RECNX_ACT_NONE. The caller then drives scs_csb_reconnect_tick() once
 *     per loop; the once-a-second reconnect beat and the period expiry live
 *     there. (transcript p. 7-30.)
 *
 * No-op returning NONE if csb is NULL or is_local.
 */
enum scs_recnx_action scs_csb_connectivity_lost(struct scs_csb *csb, uint64_t now_ms,
                                                int last_gasp);

/*
 * scs_csb_reconnect_tick - advance the RECNXINTERVAL reconnect loop (transcript
 * p. 7-30). Acts only in WAIT or RECONNECT; returns NONE in every other state.
 *
 *   - If the reconnect period (scs_recnx_timeout_secs) has fully elapsed since
 *     connectivity was lost: all attempts have failed. If peer_transition_started
 *     is 0 the local CM starts the transition -> state DISCONNECT, return
 *     PROPOSE_TRANSITION. If a peer has ALREADY started one, the local CM does
 *     NOT start its own -> return NONE (the book's "if no other Connection
 *     Manager has already instituted a cluster state transition").
 *   - Otherwise, once per SCS_RECNX_ATTEMPT_INTERVAL_MS since the last attempt:
 *     state RECONNECT, bump attempts, membership still HELD, return RECONNECT.
 *   - Otherwise: NONE (still inside the current one-second beat).
 *
 * A now_ms that appears to run backwards relative to the recorded stamps fires
 * nothing. No-op returning NONE if csb is NULL.
 */
enum scs_recnx_action scs_csb_reconnect_tick(struct scs_csb *csb, uint64_t now_ms,
                                             int peer_transition_started);

/* 1 iff this CSB currently counts as a held cluster member. 0 for NULL. */
int scs_csb_is_member(const struct scs_csb *csb);

/* Human-readable names, for logs and tests. Never NULL. */
const char *scs_csb_state_name(enum scs_csb_conn_state s);
const char *scs_recnx_action_name(enum scs_recnx_action a);

#ifdef __cplusplus
}
#endif

#endif /* SCS_RECNX_H */
