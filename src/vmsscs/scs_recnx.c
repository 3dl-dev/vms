/*
 * scs_recnx.c - Connection Manager CSB connectivity state machine + the
 * RECNXINTERVAL VC-breakage reconnect loop (vms-c7d).
 *
 * Grounding is quoted at each site. The rules come from the mined VAXcluster
 * internals transcript (host-only): the ten connectivity states from
 * ch7-part02 pp. 7-23/7-24, and the once-a-second reconnect loop with its
 * max(RECNXINTERVAL, remote port value) period from p. 7-30. See scs_recnx.h
 * for the full clean-room provenance (CLAUDE.md rule 8).
 */
#include "scs_recnx.h"

#include <stdlib.h>
#include <string.h>

void scs_csb_init(struct scs_csb *csb, int is_local, unsigned local_recnxinterval)
{
    if (csb == NULL) {
        return;
    }
    memset(csb, 0, sizeof(*csb));
    csb->is_local = is_local ? 1 : 0;
    /* p. 7-24: LOCAL "is reserved for the CSB representing the local Connection
     * Manager"; every other CSB starts NEW ("just been allocated"). */
    csb->state = is_local ? SCS_CSB_LOCAL : SCS_CSB_NEW;
    csb->local_recnxinterval =
        (local_recnxinterval != 0u) ? local_recnxinterval : SCS_RECNX_DEFAULT_RECNXINTERVAL;
}

void scs_csb_set_remote_port_value(struct scs_csb *csb, unsigned secs)
{
    if (csb == NULL) {
        return;
    }
    csb->remote_port_value = secs;
}

unsigned scs_recnx_lan_remote_value(int v55_or_later, unsigned remote_timvcfail)
{
    /* p. 7-30: for a LAN virtual circuit the remote value "was fixed at 16 prior
     * to Version 5.5. But starting with Version 5.5, the remote system supplies
     * the value of its TIMVCFAIL parameter." */
    return v55_or_later ? remote_timvcfail : SCS_RECNX_LAN_PRE_V55_VALUE;
}

unsigned scs_recnx_timeout_secs(unsigned local_recnxinterval, unsigned remote_port_value)
{
    /* p. 7-30: "the maximum of the local value for RECNXINTERVAL and a port
     * dependent number supplied by the remote Connection Manager." */
    return (local_recnxinterval >= remote_port_value) ? local_recnxinterval
                                                       : remote_port_value;
}

int scs_csb_reconnect_enabled(void)
{
    /* Read fresh every call (see scs_recnx.h): a cached answer could not be
     * bracketed by a test, and guardrail 23 requires the switch be RUN. */
    const char *v = getenv(SCS_RECNX_NO_RECONNECT_ENV);
    if (v == NULL || v[0] == '\0') {
        return 1;
    }
    if (v[0] == '0' && v[1] == '\0') {
        return 1;
    }
    return 0; /* set to anything else -> reconnect disabled (pre-vms-c7d behaviour) */
}

int scs_csb_is_member(const struct scs_csb *csb)
{
    return (csb != NULL) ? csb->member : 0;
}

void scs_csb_connectivity_gained(struct scs_csb *csb, uint64_t now_ms)
{
    (void)now_ms;
    if (csb == NULL || csb->is_local) {
        return;
    }
    /* A connection that was being timed/reconnected and is now open is a
     * successful reconnection (p. 7-24 WAIT/RECONNECT -> OPEN). */
    if (csb->state == SCS_CSB_WAIT || csb->state == SCS_CSB_RECONNECT ||
        csb->state == SCS_CSB_REACCEPT) {
        csb->reconnects++;
    }
    /* p. 7-24: OPEN is "the normal state of a CSB." Membership is held. */
    csb->state = SCS_CSB_OPEN;
    csb->member = 1;
    csb->lost_ms = 0;
    csb->last_attempt_ms = 0;
    csb->attempts = 0;
}

enum scs_recnx_action scs_csb_connectivity_lost(struct scs_csb *csb, uint64_t now_ms,
                                                int last_gasp)
{
    if (csb == NULL || csb->is_local) {
        return SCS_RECNX_ACT_NONE;
    }

    if (last_gasp) {
        /* p. 7-29: a "last gasp" datagram (BUGCHECK / SHUTDOWN.COM) means the
         * remote has ANNOUNCED it is leaving -- the port driver closes the VC
         * immediately and the Connection Manager initiates a transition. There
         * is nothing to wait for, so no WAIT and no reconnect. */
        csb->state = SCS_CSB_DISCONNECT;
        csb->member = 0;
        csb->transitions_proposed++;
        return SCS_RECNX_ACT_PROPOSE_TRANSITION;
    }

    if (!scs_csb_reconnect_enabled()) {
        /* THE KILL SWITCH / pre-vms-c7d behaviour: a broken circuit drops
         * membership at once and proposes a transition, with no timed reconnect.
         * This is the FAIL-pre state the reconnect loop replaces. */
        csb->state = SCS_CSB_DISCONNECT;
        csb->member = 0;
        csb->transitions_proposed++;
        return SCS_RECNX_ACT_PROPOSE_TRANSITION;
    }

    /* p. 7-30: connectivity lost for a reason NOT involving a last gasp -->
     * "Do not presume that the remote system has left ... the local Connection
     * Manager will attempt once a second to establish another connection." Enter
     * WAIT and HOLD membership; the reconnect tick runs the once-a-second beat.
     * p. 7-24 WAIT: "Connectivity has been lost ... A timeout is in progress." */
    csb->state = SCS_CSB_WAIT;
    csb->lost_ms = now_ms;
    csb->last_attempt_ms = now_ms; /* first attempt one beat (1 s) from now */
    csb->attempts = 0;
    /* csb->member deliberately UNCHANGED -- the node is not dropped. */
    return SCS_RECNX_ACT_NONE;
}

enum scs_recnx_action scs_csb_reconnect_tick(struct scs_csb *csb, uint64_t now_ms,
                                             int peer_transition_started)
{
    if (csb == NULL) {
        return SCS_RECNX_ACT_NONE;
    }
    if (csb->state != SCS_CSB_WAIT && csb->state != SCS_CSB_RECONNECT) {
        return SCS_RECNX_ACT_NONE;
    }
    /* Guard a clock that appears to run backwards relative to the recorded loss
     * time -- fire nothing (mirrors scs_vc_retransmit_due). */
    if (now_ms < csb->lost_ms) {
        return SCS_RECNX_ACT_NONE;
    }

    const uint64_t period_ms =
        (uint64_t)scs_recnx_timeout_secs(csb->local_recnxinterval,
                                         csb->remote_port_value) * 1000u;
    const uint64_t elapsed = now_ms - csb->lost_ms;

    if (elapsed >= period_ms) {
        /* p. 7-30: "If all such reconnect attempts fail, and if no other
         * Connection Manager has already instituted a cluster state transition,
         * then the local Connection Manager starts a state transition." */
        if (peer_transition_started) {
            return SCS_RECNX_ACT_NONE; /* another CM already started one */
        }
        csb->state = SCS_CSB_DISCONNECT;
        csb->transitions_proposed++;
        return SCS_RECNX_ACT_PROPOSE_TRANSITION;
    }

    /* Inside the period: once a second (p. 7-30 "attempt once a second"). */
    if (now_ms < csb->last_attempt_ms) {
        return SCS_RECNX_ACT_NONE;
    }
    if ((now_ms - csb->last_attempt_ms) >= SCS_RECNX_ATTEMPT_INTERVAL_MS) {
        csb->last_attempt_ms = now_ms;
        csb->attempts++;
        csb->state = SCS_CSB_RECONNECT; /* p. 7-24: "a reconnect attempt ... is in progress" */
        /* Membership still HELD -- see scs_csb_is_member. */
        return SCS_RECNX_ACT_RECONNECT;
    }
    return SCS_RECNX_ACT_NONE;
}

const char *scs_csb_state_name(enum scs_csb_conn_state s)
{
    switch (s) {
    case SCS_CSB_NEW:        return "NEW";
    case SCS_CSB_CONNECT:    return "CONNECT";
    case SCS_CSB_ACCEPT:     return "ACCEPT";
    case SCS_CSB_OPEN:       return "OPEN";
    case SCS_CSB_DISCONNECT: return "DISCONNECT";
    case SCS_CSB_WAIT:       return "WAIT";
    case SCS_CSB_RECONNECT:  return "RECONNECT";
    case SCS_CSB_REACCEPT:   return "REACCEPT";
    case SCS_CSB_DEAD:       return "DEAD";
    case SCS_CSB_LOCAL:      return "LOCAL";
    default:                 return "?";
    }
}

const char *scs_recnx_action_name(enum scs_recnx_action a)
{
    switch (a) {
    case SCS_RECNX_ACT_NONE:               return "none";
    case SCS_RECNX_ACT_RECONNECT:          return "reconnect attempt";
    case SCS_RECNX_ACT_PROPOSE_TRANSITION: return "propose state transition";
    default:                               return "?";
    }
}
