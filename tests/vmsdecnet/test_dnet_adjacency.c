/*
 * test_dnet_adjacency.c - deterministic unit test for the DECnet Phase IV
 *                         routing adjacency state machine (rd vms-b15, rung 3).
 *
 * The SM is clock-injectable: every step passes an explicit monotonic tick, so
 * this test drives the whole lifecycle with a hand-stepped counter -- no wall
 * clock, no sleep, fully deterministic.
 *
 * Timer ground truth (docs/decnet-provenance-register.md sec 4.6, rd vms-3be):
 * the captured lab-VAX endnode HELLO advertised "hello 15" => T3 = 15 s. The
 * listen timer is T4 = BCT3MULT * T3; BCT3MULT is the DNA-spec default (2, not
 * oracle-captured -- no router-timeout specimen was captured). Here we use T3=15
 * and BCT3MULT=2 => T4 = 30 ticks, matching those sources.
 *
 * Coverage (the rd vms-b15 done-conditions):
 *   1. a neighbour comes UP after a valid (two-way) HELLO;
 *   2. it stays UP while HELLOs keep arriving within T4;
 *   3. it goes DOWN when the listen timer expires with no HELLO;
 *   4. our own HELLO cadence fires at T3.
 * Plus: the intermediate INITIALIZING state for a one-way HELLO, re-up after a
 * lapse, and null-argument guards.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dnet_adjacency.h"
#include "dnet_hello.h"

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

/* Our own node: 1.1 => AA-00-04-00-01-04 (as in the vms-3be specimen). */
static const uint8_t kMyId[6]   = { 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04 };
/* A neighbour node: 1.2 => AA-00-04-00-02-04. */
static const uint8_t kPeerId[6] = { 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04 };

/* Build a decoded endnode HELLO from `src`, advertising `timer`, whose
 * neighbour field is `names` (NULL => 0.0, no router / one-way). */
static struct dnet_endnode_hello mk_hello(const uint8_t src[6], uint16_t timer,
                                          const uint8_t names[6])
{
    struct dnet_endnode_hello h;
    memset(&h, 0, sizeof(h));
    h.rflags  = DNET_RFLAG_ENDNODE_HELLO;
    h.version = 2;
    memcpy(h.id, src, 6);
    h.iinfo   = DNET_NODETYPE_ENDNODE;
    h.blksize = 1498;
    h.timer   = timer;
    if (names)
        memcpy(h.neighbor, names, 6);
    return h;
}

int main(void)
{
    printf("test_dnet_adjacency: DECnet Phase IV adjacency SM (T3=15, BCT3MULT=2)\n");

    struct dnet_adjacency sm;
    /* t3=0 and bct3mult=0 must select the oracle/spec defaults. */
    int rc = dnet_adj_init(&sm, kMyId, 0, 0, /*now=*/0);
    check(rc == DNET_ADJ_OK, "dnet_adj_init succeeds");
    check(sm.t3 == DNET_T3_DEFAULT, "T3 defaults to 15 (oracle vms-3be)");
    check(sm.bct3mult == DNET_BCT3MULT_DEFAULT, "BCT3MULT defaults to 2 (DNA spec)");
    const dnet_tick_t T4 = (dnet_tick_t)sm.bct3mult * DNET_T3_DEFAULT; /* 30 */
    check(T4 == 30, "listen timer T4 == BCT3MULT * T3 == 30");

    /* Unknown neighbour is DOWN. */
    check(dnet_adj_state_of(&sm, kPeerId) == DNET_ADJ_DOWN,
          "unheard neighbour reads DOWN");

    /* --- (intermediate state) a ONE-WAY hello lands in INITIALIZING --- */
    struct dnet_endnode_hello oneway = mk_hello(kPeerId, 15, NULL);
    enum dnet_adj_state st = DNET_ADJ_DOWN;
    rc = dnet_adj_rx_hello(&sm, /*now=*/10, &oneway, &st);
    check(rc == DNET_ADJ_OK, "rx one-way HELLO succeeds");
    check(st == DNET_ADJ_INITIALIZING, "one-way HELLO => INITIALIZING (not yet two-way)");
    check(dnet_adj_state_of(&sm, kPeerId) == DNET_ADJ_INITIALIZING,
          "neighbour state reads INITIALIZING");

    /* --- (1) a TWO-WAY hello (peer names us) brings the neighbour UP --- */
    struct dnet_endnode_hello twoway = mk_hello(kPeerId, 15, kMyId);
    rc = dnet_adj_rx_hello(&sm, /*now=*/12, &twoway, &st);
    check(rc == DNET_ADJ_OK && st == DNET_ADJ_UP,
          "two-way HELLO (peer names us) => adjacency UP");
    struct dnet_adj_neighbor *n = dnet_adj_find(&sm, kPeerId);
    check(n != NULL, "neighbour is tracked");
    check(n->listen_deadline == 12 + T4, "listen timer armed to now + T4 (== 42)");
    check(n->addr == 0x0402 /* node 1.2, area<<10|node */,
          "neighbour DECnet address decoded (1.2)");

    /* --- (2) HELLOs arriving within T4 keep it UP; a tick short of the --- *
     * --- deadline does NOT down it -------------------------------------- */
    dnet_tick_t t = 12;
    for (int i = 0; i < 3; i++) {
        t += 20;                              /* 20 < T4=30: always in-window */
        struct dnet_endnode_hello keep = mk_hello(kPeerId, 15, kMyId);
        rc = dnet_adj_rx_hello(&sm, t, &keep, &st);
        check(rc == DNET_ADJ_OK && st == DNET_ADJ_UP, "in-window HELLO keeps adjacency UP");
        /* a tick just before the (refreshed) deadline must not expire it */
        int downed = dnet_adj_tick(&sm, t + (T4 - 1));
        check(downed == 0 && dnet_adj_state_of(&sm, kPeerId) == DNET_ADJ_UP,
              "tick just inside T4 leaves adjacency UP");
    }

    /* --- (3) when HELLOs stop, the listen timer expires it to DOWN --- */
    n = dnet_adj_find(&sm, kPeerId);
    dnet_tick_t deadline = n->listen_deadline;
    int downed = dnet_adj_tick(&sm, deadline - 1);
    check(downed == 0 && dnet_adj_state_of(&sm, kPeerId) == DNET_ADJ_UP,
          "one tick before the deadline: still UP");
    downed = dnet_adj_tick(&sm, deadline);      /* now >= deadline */
    check(downed == 1, "listen-timer expiry downs exactly one neighbour");
    check(dnet_adj_state_of(&sm, kPeerId) == DNET_ADJ_DOWN,
          "expired adjacency reads DOWN (no HELLO within T4)");
    /* a second tick does not re-count an already-DOWN neighbour */
    check(dnet_adj_tick(&sm, deadline + 100) == 0, "already-DOWN neighbour not re-downed");

    /* --- re-up: a fresh HELLO after a lapse rebuilds the adjacency --- */
    struct dnet_endnode_hello revive = mk_hello(kPeerId, 15, kMyId);
    rc = dnet_adj_rx_hello(&sm, deadline + 200, &revive, &st);
    check(rc == DNET_ADJ_OK && st == DNET_ADJ_UP, "HELLO after a lapse re-ups the adjacency");

    /* --- (4) our OWN hello cadence fires at T3 --- */
    struct dnet_adjacency em;
    dnet_adj_init(&em, kMyId, /*t3=*/15, /*bct3mult=*/2, /*now=*/100);
    check(dnet_adj_next_hello_tick(&em) == 115, "first own HELLO due at now + T3 (115)");
    check(!dnet_adj_hello_due(&em, 114), "not due one tick early");
    check(dnet_adj_hello_due(&em, 115), "due exactly at T3");
    check(dnet_adj_hello_due(&em, 130), "still due if we are late");
    dnet_adj_hello_emitted(&em, 115);           /* emit on time */
    check(dnet_adj_next_hello_tick(&em) == 130, "cadence reschedules to +T3 (130)");
    check(!dnet_adj_hello_due(&em, 129), "not due before the next T3 boundary");
    check(dnet_adj_hello_due(&em, 130), "due again one T3 later");

    /* --- a neighbour that advertises a different T3 arms T4 from ITS timer --- */
    struct dnet_adjacency sm2;
    dnet_adj_init(&sm2, kMyId, 0, 0, 0);
    struct dnet_endnode_hello slow = mk_hello(kPeerId, /*timer=*/40, kMyId);
    dnet_adj_rx_hello(&sm2, /*now=*/5, &slow, &st);
    n = dnet_adj_find(&sm2, kPeerId);
    check(n->adv_t3 == 40 && n->listen_deadline == 5 + 2 * 40,
          "listen timer uses the neighbour's advertised T3 (2*40)");

    /* --- null-argument guards --- */
    check(dnet_adj_init(NULL, kMyId, 0, 0, 0) == DNET_ADJ_EINVAL, "init(NULL) => EINVAL");
    check(dnet_adj_rx_hello(NULL, 0, &twoway, NULL) == DNET_ADJ_EINVAL, "rx(NULL sm) => EINVAL");
    check(dnet_adj_rx_hello(&sm, 0, NULL, NULL) == DNET_ADJ_EINVAL, "rx(NULL hello) => EINVAL");
    check(dnet_adj_tick(NULL, 0) == DNET_ADJ_EINVAL, "tick(NULL) => EINVAL");
    check(dnet_adj_state_of(NULL, kPeerId) == DNET_ADJ_DOWN, "state_of(NULL) => DOWN");

    if (failures == 0) {
        printf("test_dnet_adjacency: ALL CHECKS PASSED\n");
        return 0;
    }
    printf("test_dnet_adjacency: %d CHECK(S) FAILED\n", failures);
    return 1;
}
