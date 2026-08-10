/*
 * test_scs_recnx.c - unit tests for the vms-c7d Connection Manager CSB
 * connectivity state machine and the RECNXINTERVAL VC-breakage reconnect loop
 * (src/vmsscs/scs_recnx.c).
 *
 * WHAT IS GROUNDED. Every state name is transcribed from the mined VAXcluster
 * internals transcript (host-only) ch7-part02 pp. 7-23/7-24 (the ten CSB
 * connectivity states). Every timing rule is from p. 7-30: reconnect "once a
 * second" for a period of max(local RECNXINTERVAL, remote port value), where the
 * LAN remote value is 16 before V5.5 and the remote TIMVCFAIL from V5.5; the
 * worked example (local 20, remote 10 -> 20) is reproduced verbatim. No VSI/HPE
 * source or binary was read (CLAUDE.md rule 8).
 *
 * WHAT THESE TESTS PROVE, AND HOW. The reconnect loop is a pure function of
 * (CSB state, injected monotonic-ms clock). The fail-pre/pass-post proof drives
 * it by INJECTING a virtual-circuit breakage (scs_csb_connectivity_lost) and
 * then INJECTING a clock into scs_csb_reconnect_tick -- it never hand-sets the
 * state next to an assertion. The two arms:
 *   - PASS-post (default): a breakage enters WAIT, HOLDS membership, and the
 *     tick emits a reconnect attempt once per second for the whole period, then
 *     proposes a transition at expiry (or reaches OPEN with membership never
 *     dropped if the circuit is restored).
 *   - FAIL-pre (OVMX_NO_RECNX_RECONNECT=1, guardrail 23, RUN not asserted): the
 *     SAME breakage drops membership immediately and proposes a transition with
 *     no WAIT and no timed reconnect -- the behaviour this item replaces.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scs_recnx.h"

static int failures = 0;

static void check(int cond, const char *what)
{
    if (cond) {
        printf("  OK: %s\n", what);
    } else {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

/* ---- the RECNXINTERVAL period arithmetic (transcript p. 7-30) ---- */
static void test_timeout_period(void)
{
    printf("[recnx] the reconnect period is max(local RECNXINTERVAL, remote value) (p. 7-30)\n");
    /* The book's worked example, verbatim: local 20, remote 10 -> 20. */
    check(scs_recnx_timeout_secs(20, 10) == 20, "max(20,10) == 20 (the p. 7-30 worked example)");
    check(scs_recnx_timeout_secs(10, 20) == 20, "max(10,20) == 20 (remote larger)");
    check(scs_recnx_timeout_secs(16, 10) == 16, "max(16,10) == 16");
    check(scs_recnx_timeout_secs(5, 5) == 5, "max(5,5) == 5");
    check(scs_recnx_timeout_secs(0, 0) == 0, "max(0,0) == 0");

    /* The LAN remote value: 16 before V5.5, remote TIMVCFAIL from V5.5. */
    check(scs_recnx_lan_remote_value(0, 10) == SCS_RECNX_LAN_PRE_V55_VALUE,
          "LAN remote value pre-V5.5 is fixed at 16 (ignores TIMVCFAIL)");
    check(scs_recnx_lan_remote_value(0, 10) == 16, "and that constant is 16");
    check(scs_recnx_lan_remote_value(1, 10) == 10,
          "LAN remote value from V5.5 is the remote TIMVCFAIL (10)");
    /* Compose them into the book's full example: V5.5-1, local RECNXINTERVAL 20,
     * remote TIMVCFAIL 10 -> 20 s. */
    check(scs_recnx_timeout_secs(20, scs_recnx_lan_remote_value(1, 10)) == 20,
          "the full p. 7-30 example (local 20, remote TIMVCFAIL 10) -> 20 s");
}

/* ---- the ten connectivity states exist and are named (pp. 7-23/7-24) ---- */
static void test_state_names(void)
{
    printf("[recnx] the ten CSB connectivity states (pp. 7-23/7-24)\n");
    const enum scs_csb_conn_state all[10] = {
        SCS_CSB_NEW, SCS_CSB_CONNECT, SCS_CSB_ACCEPT, SCS_CSB_OPEN,
        SCS_CSB_DISCONNECT, SCS_CSB_WAIT, SCS_CSB_RECONNECT, SCS_CSB_REACCEPT,
        SCS_CSB_DEAD, SCS_CSB_LOCAL
    };
    const char *want[10] = { "NEW", "CONNECT", "ACCEPT", "OPEN", "DISCONNECT",
                             "WAIT", "RECONNECT", "REACCEPT", "DEAD", "LOCAL" };
    int ok = 1;
    for (int i = 0; i < 10; i++) {
        if (strcmp(scs_csb_state_name(all[i]), want[i]) != 0) {
            ok = 0;
        }
    }
    check(ok, "all ten states are named exactly as the transcript lists them");
    check(scs_recnx_action_name(SCS_RECNX_ACT_RECONNECT) != NULL &&
          scs_recnx_action_name(SCS_RECNX_ACT_PROPOSE_TRANSITION) != NULL,
          "action names are never NULL");
}

/* ---- init: local CSB vs a discovered remote CSB ---- */
static void test_init(void)
{
    printf("[recnx] init: LOCAL vs NEW (p. 7-24)\n");
    struct scs_csb local;
    scs_csb_init(&local, /*is_local=*/1, 20);
    check(local.state == SCS_CSB_LOCAL, "is_local -> the LOCAL CSB");
    check(local.is_local == 1, "is_local flag set");

    struct scs_csb remote;
    scs_csb_init(&remote, 0, 20);
    check(remote.state == SCS_CSB_NEW, "a discovered remote CM starts NEW");
    check(remote.member == 0, "a NEW CSB is not yet a held member");
    check(remote.local_recnxinterval == 20, "local RECNXINTERVAL recorded");

    struct scs_csb def;
    scs_csb_init(&def, 0, 0);
    check(def.local_recnxinterval == SCS_RECNX_DEFAULT_RECNXINTERVAL,
          "RECNXINTERVAL 0 falls back to the OVMX default");

    /* The LOCAL CSB is never a reconnect subject. */
    check(scs_csb_connectivity_lost(&local, 100, 0) == SCS_RECNX_ACT_NONE,
          "the LOCAL CSB ignores connectivity-lost");
    check(local.state == SCS_CSB_LOCAL, "the LOCAL CSB stays LOCAL");
}

/*
 * THE CORE PROOF (pass-post): an INJECTED VC breakage enters WAIT and HOLDS
 * membership; the tick, driven by an INJECTED clock at 250 ms granularity,
 * emits a reconnect attempt exactly once per second for the whole
 * max(RECNXINTERVAL, remote)=3 s period, then proposes a transition at expiry.
 */
static void test_reconnect_cadence_and_membership_held(void)
{
    printf("[recnx] PASS-post: 1/sec reconnect for max(RECNX,remote), membership HELD\n");
    /* Ensure the default (reconnect enabled) environment. */
    unsetenv(SCS_RECNX_NO_RECONNECT_ENV);

    struct scs_csb csb;
    scs_csb_init(&csb, 0, /*RECNXINTERVAL=*/3);
    scs_csb_set_remote_port_value(&csb, 2); /* period = max(3,2) = 3 s */

    /* The circuit is open and this node is a held member. */
    scs_csb_connectivity_gained(&csb, 0);
    check(csb.state == SCS_CSB_OPEN && csb.member == 1,
          "precondition: circuit OPEN, node is a member");

    /* INJECT the VC breakage (not a last gasp). */
    enum scs_recnx_action a = scs_csb_connectivity_lost(&csb, 0, /*last_gasp=*/0);
    check(a == SCS_RECNX_ACT_NONE, "breakage returns NONE -- it starts a wait, not a transition");
    check(csb.state == SCS_CSB_WAIT, "breakage -> WAIT (a timeout is in progress, p. 7-24)");
    check(csb.member == 1, "MEMBERSHIP HELD across the breakage (p. 7-30: do not presume departure)");

    /* Drive an INJECTED clock at 250 ms steps across the whole 3 s period. */
    unsigned reconnects = 0;
    uint64_t reconnect_at[8];
    int membership_ever_dropped = 0;
    enum scs_recnx_action propose = SCS_RECNX_ACT_NONE;
    uint64_t propose_at = 0;

    for (uint64_t t = 250; t <= 3000; t += 250) {
        enum scs_recnx_action act = scs_csb_reconnect_tick(&csb, t, /*peer_transition=*/0);
        if (act == SCS_RECNX_ACT_RECONNECT) {
            if (reconnects < 8) {
                reconnect_at[reconnects] = t;
            }
            reconnects++;
        } else if (act == SCS_RECNX_ACT_PROPOSE_TRANSITION) {
            propose = act;
            propose_at = t;
        }
        /* Until the transition is proposed, the node must stay a held member. */
        if (act != SCS_RECNX_ACT_PROPOSE_TRANSITION && !csb.member) {
            membership_ever_dropped = 1;
        }
    }

    check(reconnects == 2, "exactly 2 reconnect attempts fired in a 3 s period (once per second)");
    check(reconnects == 2 && reconnect_at[0] == 1000 && reconnect_at[1] == 2000,
          "the attempts landed at t=1000 and t=2000 -- the once-a-second beat, never faster");
    check(csb.attempts == 2, "the CSB counted exactly 2 attempts");
    check(!membership_ever_dropped, "membership was HELD the entire time the reconnect ran");
    check(propose == SCS_RECNX_ACT_PROPOSE_TRANSITION && propose_at == 3000,
          "at the period's expiry (t=3000) the local CM proposes a state transition (p. 7-30)");
    check(csb.state == SCS_CSB_DISCONNECT, "after proposing, the CSB leaves WAIT/RECONNECT");
    check(csb.transitions_proposed == 1, "exactly one transition was proposed");

    /* Backwards clock fires nothing. */
    struct scs_csb b;
    scs_csb_init(&b, 0, 3);
    scs_csb_connectivity_gained(&b, 10000);
    scs_csb_connectivity_lost(&b, 10000, 0);
    check(scs_csb_reconnect_tick(&b, 5000, 0) == SCS_RECNX_ACT_NONE,
          "a clock that appears to run backwards fires nothing");
}

/*
 * Recovery: the circuit is restored mid-period -> OPEN, membership never
 * dropped, no transition, the reconnection counted.
 */
static void test_reconnect_recovers_without_membership_loss(void)
{
    printf("[recnx] PASS-post: VC restored mid-period -> OPEN, membership never lost\n");
    unsetenv(SCS_RECNX_NO_RECONNECT_ENV);

    struct scs_csb csb;
    scs_csb_init(&csb, 0, 3);
    scs_csb_set_remote_port_value(&csb, 0); /* period = max(3,0) = 3 s */
    scs_csb_connectivity_gained(&csb, 0);
    scs_csb_connectivity_lost(&csb, 0, 0);

    /* One reconnect beat fires... */
    check(scs_csb_reconnect_tick(&csb, 1000, 0) == SCS_RECNX_ACT_RECONNECT,
          "a reconnect attempt fires at t=1000");
    check(csb.state == SCS_CSB_RECONNECT, "state is RECONNECT while the attempt is in progress");
    check(csb.member == 1, "still a held member during the attempt");

    /* ...and the circuit comes back before the period expires. */
    scs_csb_connectivity_gained(&csb, 1500);
    check(csb.state == SCS_CSB_OPEN, "circuit restored -> OPEN");
    check(csb.member == 1, "membership was HELD throughout -- never dropped");
    check(csb.reconnects == 1, "the successful reconnection was counted");

    /* No later tick proposes a transition on a healthy circuit. */
    check(scs_csb_reconnect_tick(&csb, 100000, 0) == SCS_RECNX_ACT_NONE,
          "an OPEN circuit proposes no transition, however long the clock runs");
    check(csb.transitions_proposed == 0, "no transition was ever proposed");
}

/*
 * p. 7-30: "if no other Connection Manager has already instituted a cluster
 * state transition" -- if a peer already started one, the local CM does NOT
 * start its own at expiry.
 */
static void test_peer_transition_suppresses_our_own(void)
{
    printf("[recnx] a peer-started transition suppresses our own at expiry (p. 7-30)\n");
    unsetenv(SCS_RECNX_NO_RECONNECT_ENV);

    struct scs_csb csb;
    scs_csb_init(&csb, 0, 2);
    scs_csb_set_remote_port_value(&csb, 0); /* period = 2 s */
    scs_csb_connectivity_gained(&csb, 0);
    scs_csb_connectivity_lost(&csb, 0, 0);

    /* At expiry, a peer has ALREADY started a transition -> we start none. */
    check(scs_csb_reconnect_tick(&csb, 2000, /*peer_transition_started=*/1) == SCS_RECNX_ACT_NONE,
          "expiry with a peer transition already running -> we propose NOTHING");
    check(csb.transitions_proposed == 0, "and no transition is charged to us");

    /* With no peer transition, the same expiry DOES propose. */
    check(scs_csb_reconnect_tick(&csb, 2000, 0) == SCS_RECNX_ACT_PROPOSE_TRANSITION,
          "expiry with no peer transition -> we propose one");
    check(csb.transitions_proposed == 1, "and it is charged to us");
}

/*
 * p. 7-29: a "last gasp" datagram is a departure announcement -- close the
 * circuit and reconfigure immediately; NO reconnect wait.
 */
static void test_last_gasp_skips_reconnect(void)
{
    printf("[recnx] a last-gasp departure skips the reconnect loop (p. 7-29)\n");
    unsetenv(SCS_RECNX_NO_RECONNECT_ENV);

    struct scs_csb csb;
    scs_csb_init(&csb, 0, 20);
    scs_csb_connectivity_gained(&csb, 0);
    check(csb.member == 1, "member before the last gasp");

    enum scs_recnx_action a = scs_csb_connectivity_lost(&csb, 100, /*last_gasp=*/1);
    check(a == SCS_RECNX_ACT_PROPOSE_TRANSITION,
          "a last gasp proposes a transition immediately");
    check(csb.state == SCS_CSB_DISCONNECT, "last gasp -> DISCONNECT, not WAIT");
    check(csb.member == 0, "last gasp drops membership at once (the node announced it is leaving)");
    check(scs_csb_reconnect_tick(&csb, 5000, 0) == SCS_RECNX_ACT_NONE,
          "no reconnect beat ever fires after a last gasp");
    check(csb.attempts == 0, "and no reconnect attempt was made");
}

/*
 * FAIL-pre (guardrail 23, RUN): with OVMX_NO_RECNX_RECONNECT=1 the SAME
 * non-last-gasp breakage drops membership immediately with no WAIT and no timed
 * reconnect -- exactly the pre-vms-c7d behaviour this item replaces.
 */
static void test_kill_switch_reverts_to_immediate_drop(void)
{
    printf("[recnx] FAIL-pre: OVMX_NO_RECNX_RECONNECT=1 -> immediate drop, no reconnect\n");

    /* The switch's own truth table, RUN. */
    unsetenv(SCS_RECNX_NO_RECONNECT_ENV);
    check(scs_csb_reconnect_enabled() == 1, "switch unset -> reconnect enabled");
    setenv(SCS_RECNX_NO_RECONNECT_ENV, "0", 1);
    check(scs_csb_reconnect_enabled() == 1, "switch=0 -> reconnect enabled");
    setenv(SCS_RECNX_NO_RECONNECT_ENV, "1", 1);
    check(scs_csb_reconnect_enabled() == 0, "switch=1 -> reconnect DISABLED");

    struct scs_csb csb;
    scs_csb_init(&csb, 0, 3);
    scs_csb_set_remote_port_value(&csb, 2);
    scs_csb_connectivity_gained(&csb, 0);
    check(csb.member == 1, "precondition: a held member");

    /* The identical breakage the pass-post test injected -- but now the pre-fix
     * behaviour must show: immediate transition, membership dropped, no WAIT. */
    enum scs_recnx_action a = scs_csb_connectivity_lost(&csb, 0, /*last_gasp=*/0);
    check(a == SCS_RECNX_ACT_PROPOSE_TRANSITION,
          "kill switch: a plain breakage proposes a transition IMMEDIATELY");
    check(csb.state != SCS_CSB_WAIT, "kill switch: the node NEVER enters WAIT");
    check(csb.member == 0, "kill switch: membership is DROPPED at once (the fail-pre behaviour)");

    /* And no reconnect beat ever runs. */
    unsigned before = csb.attempts;
    for (uint64_t t = 1000; t <= 5000; t += 1000) {
        (void)scs_csb_reconnect_tick(&csb, t, 0);
    }
    check(csb.attempts == before, "kill switch: NO 1/sec reconnect attempt ever fires");

    unsetenv(SCS_RECNX_NO_RECONNECT_ENV);
}

/* NULL / edge safety across the surface. */
static void test_null_safety(void)
{
    printf("[recnx] NULL safety\n");
    scs_csb_init(NULL, 0, 20);
    scs_csb_set_remote_port_value(NULL, 5);
    scs_csb_connectivity_gained(NULL, 1);
    check(scs_csb_connectivity_lost(NULL, 1, 0) == SCS_RECNX_ACT_NONE, "lost(NULL) -> NONE");
    check(scs_csb_reconnect_tick(NULL, 1, 0) == SCS_RECNX_ACT_NONE, "tick(NULL) -> NONE");
    check(scs_csb_is_member(NULL) == 0, "is_member(NULL) -> 0");
    check(scs_csb_state_name((enum scs_csb_conn_state)999) != NULL, "state_name is never NULL");
    check(scs_recnx_action_name((enum scs_recnx_action)999) != NULL, "action_name is never NULL");

    /* A tick in a non-WAIT/RECONNECT state is inert. */
    struct scs_csb csb;
    scs_csb_init(&csb, 0, 20);
    check(scs_csb_reconnect_tick(&csb, 5000, 0) == SCS_RECNX_ACT_NONE,
          "a tick in NEW does nothing");
    scs_csb_connectivity_gained(&csb, 0);
    check(scs_csb_reconnect_tick(&csb, 5000, 0) == SCS_RECNX_ACT_NONE,
          "a tick in OPEN does nothing");
}

int main(void)
{
    printf("=== test_scs_recnx: CSB connectivity states + RECNXINTERVAL reconnect (vms-c7d) ===\n");
    test_timeout_period();
    test_state_names();
    test_init();
    test_reconnect_cadence_and_membership_held();
    test_reconnect_recovers_without_membership_loss();
    test_peer_transition_suppresses_our_own();
    test_last_gasp_skips_reconnect();
    test_kill_switch_reverts_to_immediate_drop();
    test_null_safety();

    if (failures == 0) {
        printf("\nALL PASS\n");
        return 0;
    }
    printf("\n%d FAILURE(S)\n", failures);
    return 1;
}
