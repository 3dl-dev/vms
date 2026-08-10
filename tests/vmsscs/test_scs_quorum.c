/*
 * test_scs_quorum.c - unit tests for the connection-manager CEVOTES/QUORUM
 * computation and the quorum gate (vms-7a9).
 *
 * WHY A UNIT TEST AND NOT A WIRE ASSERTION. The quorum arithmetic is NOT
 * wire-observable: every reference-lab capture ran a single vote configuration
 * (VOTES 1/0/0, EXPECTED_VOTES held at 1, F$GETSYI QUORUM=1), so there is no
 * wire contrast to bind the quorum value to any byte (vms-41d). The DYNAMICS
 * therefore come from the DOCUMENTED ALGORITHM, and the honest proof is a
 * known-answer test against a worked example -- NOT a fabricated wire value.
 *
 * ORACLE (documented, clean-room rule 8):
 *   - VMScluster Systems for OpenVMS, sec 2.3.5 "Calculating Cluster Votes":
 *       New quorum = max{ current quorum ; (EXPECTED_VOTES+2)/2 ;
 *                         (SUM VOTES+2)/2 }, rounded down; never decreases.
 *     sec 2.3.6 the 3-node worked example (VOTES 1 each, EXPECTED_VOTES 3 ->
 *     quorum 2); sec 2.3.8 the two-node + quorum-disk example.
 *   - The mined CM transcript ch7-part01 pp. 7-5..7-7: the identical rule stated
 *     as New CEVOTES = max{EXPECTED_VOTES; SUM VOTES; Old CEVOTES},
 *     QUORUM = (New CEVOTES + 2)/2, and the 5-NODE worked example reproduced
 *     below (HOST-ONLY transcript, cited by page, never committed).
 *   - vms-2d6: quorum-loss BEHAVIOUR -- survivors suspend activity and WAIT; the
 *     connection manager does NOT reconfigure while quorum is lost.
 *
 * The one wire-grounded piece -- the per-node VOTES field at the 190-byte VC
 * body[22:24] (spec sec 4j) -- gets a real frame round-trip assertion at the end
 * (build_params emits it, scs_member_parse consumes it), because THAT part is
 * observable and grounded across four vote configurations.
 */
#include "scs_quorum.h"
#include "scs_member.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++;                                                       \
        }                                                                     \
    } while (0)

/*
 * THE 5-NODE WORKED EXAMPLE (ch7-part01 pp. 7-5..7-7; cross-checked against
 * VMScluster Systems sec 2.3.5). Five nodes, each VOTES=1, EXPECTED_VOTES=5.
 * As nodes leave, CEVOTES (hence QUORUM) never decreases; present votes fall
 * until quorum is lost.
 *
 *   transition            up  SUM  maxEXP  Old   New CEVOTES  QUORUM  present  gate
 *   ------------------------------------------------------------------------------
 *   1. all five formed    5    5     5      0       5           3       5      RUN
 *   2. one departs        4    5     5      5       5           3       4      RUN
 *   3. two departed       3    5     5      5       5           3       3      RUN (==)
 *   4. three departed     2    5     5      5       5           3       2      BLOCK
 *
 * QUORUM = (5 + 2)/2 = 3 throughout; the boundary is present==quorum (RUN) vs
 * present<quorum (BLOCK).
 */
static void test_five_node_worked_example(void)
{
    struct scs_quorum q;
    scs_quorum_init(&q);

    const uint32_t node[5] = {1025, 1026, 1027, 1028, 1029};
    for (int i = 0; i < 5; i++) {
        CHECK(scs_quorum_set_member(&q, node[i], /*votes=*/1,
                                    /*expected=*/5, /*up=*/1) == 0,
              "5-node: add member");
    }

    /* Transition 1: all five formed. */
    scs_quorum_recompute(&q);
    CHECK(q.cevotes == 5, "5-node t1: CEVOTES = max{5,5,0} = 5");
    CHECK(q.quorum == 3, "5-node t1: QUORUM = (5+2)/2 = 3");
    CHECK(q.present_votes == 5, "5-node t1: present = 5");
    CHECK(scs_quorum_gate(&q) == SCS_QUORUM_RUN, "5-node t1: RUN");

    /* Transition 2: node 1029 departs. CEVOTES must NOT decrease. */
    CHECK(scs_quorum_set_up(&q, 1029, 0) == 0, "5-node t2: mark 1029 down");
    scs_quorum_recompute(&q);
    CHECK(q.cevotes == 5, "5-node t2: CEVOTES stays 5 (never decreases)");
    CHECK(q.quorum == 3, "5-node t2: QUORUM stays 3");
    CHECK(q.present_votes == 4, "5-node t2: present = 4");
    CHECK(scs_quorum_gate(&q) == SCS_QUORUM_RUN, "5-node t2: RUN (4 >= 3)");

    /* Transition 3: node 1028 also departs. present == quorum -> still RUN. */
    CHECK(scs_quorum_set_up(&q, 1028, 0) == 0, "5-node t3: mark 1028 down");
    scs_quorum_recompute(&q);
    CHECK(q.present_votes == 3, "5-node t3: present = 3");
    CHECK(q.quorum == 3, "5-node t3: QUORUM = 3");
    CHECK(scs_quorum_present(&q) == 1, "5-node t3: quorum present at boundary");
    CHECK(scs_quorum_gate(&q) == SCS_QUORUM_RUN,
          "5-node t3: RUN at the boundary (present == quorum)");

    /* Transition 4: node 1027 departs. present < quorum -> BLOCK and WAIT. */
    CHECK(scs_quorum_set_up(&q, 1027, 0) == 0, "5-node t4: mark 1027 down");
    scs_quorum_recompute(&q);
    CHECK(q.present_votes == 2, "5-node t4: present = 2");
    CHECK(q.quorum == 3, "5-node t4: QUORUM still 3 (never decreased)");
    CHECK(scs_quorum_present(&q) == 0, "5-node t4: quorum LOST");
    CHECK(scs_quorum_gate(&q) == SCS_QUORUM_BLOCK,
          "5-node t4: BLOCK (quorum lost -> suspend + wait, no reconfigure)");
}

/*
 * The 3-node worked example, VMScluster Systems sec 2.3.6: three nodes VOTES=1,
 * EXPECTED_VOTES=3 -> quorum 2, so any two constitute a quorum and no single
 * node can. This is the documented public example; it must reproduce exactly.
 */
static void test_three_node_documented_example(void)
{
    struct scs_quorum q;
    scs_quorum_init(&q);
    scs_quorum_set_member(&q, 1, 1, 3, 1);
    scs_quorum_set_member(&q, 2, 1, 3, 1);
    scs_quorum_set_member(&q, 3, 1, 3, 1);
    scs_quorum_recompute(&q);
    CHECK(q.quorum == 2, "3-node sec2.3.6: QUORUM = (3+2)/2 = 2");
    CHECK(q.present_votes == 3 && scs_quorum_gate(&q) == SCS_QUORUM_RUN,
          "3-node: three up -> RUN");

    /* One leaves: two votes, quorum 2 -> still RUN. */
    scs_quorum_set_up(&q, 3, 0);
    scs_quorum_recompute(&q);
    CHECK(q.present_votes == 2 && scs_quorum_gate(&q) == SCS_QUORUM_RUN,
          "3-node: any two constitute a quorum");

    /* Two leave: one vote < quorum 2 -> BLOCK (no single node is a quorum). */
    scs_quorum_set_up(&q, 2, 0);
    scs_quorum_recompute(&q);
    CHECK(q.present_votes == 1 && scs_quorum_gate(&q) == SCS_QUORUM_BLOCK,
          "3-node: no single node constitutes a quorum");
}

/*
 * The EXPECTED_VOTES term must be able to WIN the max even when it exceeds the
 * votes actually present -- the "estimated quorum" from step 1 of sec 2.3.5.
 * Two nodes are up with 1 vote each (SUM=2) but EXPECTED_VOTES=5, so
 * CEVOTES=5 and QUORUM=3, and the pair does NOT have quorum yet.
 */
static void test_expected_votes_dominates(void)
{
    struct scs_quorum q;
    scs_quorum_init(&q);
    scs_quorum_set_member(&q, 1, 1, 5, 1);
    scs_quorum_set_member(&q, 2, 1, 5, 1);
    scs_quorum_recompute(&q);
    CHECK(q.cevotes == 5, "expected term: CEVOTES = max{5, 2, 0} = 5");
    CHECK(q.quorum == 3, "expected term: QUORUM = 3");
    CHECK(q.present_votes == 2, "expected term: present = 2");
    CHECK(scs_quorum_gate(&q) == SCS_QUORUM_BLOCK,
          "expected term: 2 < 3 -> BLOCK until more nodes join");
}

/*
 * The SUM-VOTES term must win when the running cluster has grown past its
 * configured EXPECTED_VOTES (each node kept EXPECTED_VOTES=1 but three joined
 * with a vote each). CEVOTES = max{1, 3, 0} = 3, QUORUM = 2.
 */
static void test_sum_votes_dominates(void)
{
    struct scs_quorum q;
    scs_quorum_init(&q);
    scs_quorum_set_member(&q, 1, 1, 1, 1);
    scs_quorum_set_member(&q, 2, 1, 1, 1);
    scs_quorum_set_member(&q, 3, 1, 1, 1);
    scs_quorum_recompute(&q);
    CHECK(q.cevotes == 3, "sum term: CEVOTES = max{1, 3, 0} = 3");
    CHECK(q.quorum == 2, "sum term: QUORUM = (3+2)/2 = 2");
    CHECK(scs_quorum_gate(&q) == SCS_QUORUM_RUN, "sum term: 3 >= 2 -> RUN");
}

/*
 * The connection manager NEVER decreases CEVOTES/QUORUM on its own, even across
 * repeated recomputes after votes fall (sec 2.3.5 Note). Recompute must be
 * idempotent and monotonic-up.
 */
static void test_cevotes_never_decreases(void)
{
    struct scs_quorum q;
    scs_quorum_init(&q);
    scs_quorum_set_member(&q, 1, 2, 2, 1);
    scs_quorum_set_member(&q, 2, 2, 2, 1);
    scs_quorum_recompute(&q);
    CHECK(q.cevotes == 4 && q.quorum == 3, "monotonic: CEVOTES 4, QUORUM 3");

    /* All votes drop to zero and re-recompute repeatedly: CEVOTES holds. */
    scs_quorum_set_member(&q, 1, 0, 0, 1);
    scs_quorum_set_member(&q, 2, 0, 0, 0);
    scs_quorum_recompute(&q);
    scs_quorum_recompute(&q);
    CHECK(q.cevotes == 4, "monotonic: CEVOTES holds at 4 after votes fall");
    CHECK(q.quorum == 3, "monotonic: QUORUM holds at 3");
    CHECK(q.present_votes == 0, "monotonic: present votes now 0");
    CHECK(scs_quorum_gate(&q) == SCS_QUORUM_BLOCK, "monotonic: 0 < 3 -> BLOCK");
}

/*
 * Two-node + quorum-disk example, VMScluster Systems sec 2.3.8: two nodes
 * VOTES=1 and a quorum disk contributing 1 vote, EXPECTED_VOTES=3 -> quorum 2,
 * so the cluster survives losing either one node OR the quorum disk.
 */
static void test_quorum_disk(void)
{
    struct scs_quorum q;
    scs_quorum_init(&q);
    scs_quorum_set_member(&q, 1, 1, 3, 1);
    scs_quorum_set_member(&q, 2, 1, 3, 1);
    scs_quorum_set_disk(&q, /*qdskvotes=*/1, /*present=*/1);
    scs_quorum_recompute(&q);
    CHECK(q.cevotes == 3, "qdisk: CEVOTES = max{3, 2+1, 0} = 3");
    CHECK(q.quorum == 2, "qdisk: QUORUM = 2");
    CHECK(q.present_votes == 3 && scs_quorum_gate(&q) == SCS_QUORUM_RUN,
          "qdisk: all present -> RUN");

    /* Lose one node: 1 node + disk = 2 votes >= quorum 2 -> still RUN. */
    scs_quorum_set_up(&q, 2, 0);
    scs_quorum_recompute(&q);
    CHECK(q.present_votes == 2 && scs_quorum_gate(&q) == SCS_QUORUM_RUN,
          "qdisk: survives losing a node (node+disk = 2)");

    /* Lose the disk too: 1 vote < quorum 2 -> BLOCK. */
    scs_quorum_set_disk(&q, 1, 0);
    scs_quorum_recompute(&q);
    CHECK(q.present_votes == 1 && scs_quorum_gate(&q) == SCS_QUORUM_BLOCK,
          "qdisk: losing node AND disk loses quorum");
}

/*
 * OVMX's own advertised votes must feed the model like any peer's. OVMX joins
 * NON-VOTING (VOTES=0, design sec 8) so it can never break VAX quorum: with two
 * voting VAXes (1 each) plus OVMX (0), CEVOTES=2, QUORUM=2, and killing one VAX
 * loses quorum exactly as vms-2d6 observed -- OVMX's zero vote never props it up.
 */
static void test_ovmx_nonvoting_contribution(void)
{
    struct scs_quorum q;
    scs_quorum_init(&q);
    scs_quorum_set_member(&q, 1025, 1, 1, 1); /* VAX1 voting */
    scs_quorum_set_member(&q, 1026, 1, 1, 1); /* VAX2 voting */
    scs_quorum_set_member(&q, 1027, SCS_MEMBER_VOTES_NONVOTING, 1, 1); /* OVMX */
    scs_quorum_recompute(&q);
    CHECK(q.cevotes == 2 && q.quorum == 2, "ovmx: CEVOTES 2, QUORUM 2");
    CHECK(q.present_votes == 2 && scs_quorum_gate(&q) == SCS_QUORUM_RUN,
          "ovmx: two voting VAXes present -> RUN");

    /* Kill VAX2: 1 (VAX1) + 0 (OVMX) = 1 < quorum 2 -> BLOCK. This is vms-2d6:
     * survivors go silent and WAIT, they do not reconfigure. OVMX being up with
     * zero votes must NOT keep the cluster running. */
    scs_quorum_set_up(&q, 1026, 0);
    scs_quorum_recompute(&q);
    CHECK(q.present_votes == 1, "ovmx: only VAX1's vote remains");
    CHECK(scs_quorum_gate(&q) == SCS_QUORUM_BLOCK,
          "ovmx: quorum lost -> BLOCK; OVMX's 0 votes do not prop it up");
}

/* NULL / edge handling. */
static void test_edge(void)
{
    struct scs_quorum q;
    scs_quorum_init(&q);
    CHECK(scs_quorum_set_up(&q, 999, 1) == -1, "unknown node -> -1");
    CHECK(scs_quorum_set_member(NULL, 1, 1, 1, 1) == -1, "NULL model -> -1");
    scs_quorum_recompute(&q); /* empty model: no crash */
    CHECK(q.cevotes == 0 && q.quorum == 1 && q.present_votes == 0,
          "empty: CEVOTES 0, QUORUM (0+2)/2 = 1, present 0");
    CHECK(scs_quorum_gate(&q) == SCS_QUORUM_BLOCK, "empty: 0 < 1 -> BLOCK");

    /* Overflow: fill the table, then one more must fail cleanly. */
    struct scs_quorum big;
    scs_quorum_init(&big);
    for (uint32_t i = 0; i < SCS_QUORUM_MAX_MEMBERS; i++) {
        CHECK(scs_quorum_set_member(&big, i + 1, 1, 1, 1) == 0, "fill table");
    }
    CHECK(scs_quorum_set_member(&big, 100000, 1, 1, 1) == -1,
          "table full -> -1");
}

/*
 * WIRE-GROUNDED ROUND-TRIP. The one observable piece: the VOTES field OVMX
 * advertises at the 190-byte VC body[22:24] (spec sec 4j, grounded across four
 * vote configurations) must be exactly what scs_member_parse() consumes back and
 * what the quorum model then sees. This proves consume==advertise on the wire
 * offset -- it asserts NO quorum value (quorum is not on the wire), only the
 * grounded VOTES byte position and round-trip.
 */
static const uint8_t vax1_logical[6] = {0xaa,0x00,0x04,0x00,0x01,0x04};
static const uint8_t vax2_logical[6] = {0xaa,0x00,0x04,0x00,0x02,0x04};

static void test_votes_wire_roundtrip(void)
{
    const uint16_t vote_configs[] = {0, 1, 2}; /* the grounded values (sec 4j) */
    for (size_t i = 0; i < sizeof(vote_configs) / sizeof(vote_configs[0]); i++) {
        struct scs_member_params mp;
        memset(&mp, 0, sizeof(mp));
        memcpy(mp.dst_mac, vax1_logical, 6);
        memcpy(mp.src_mac, vax2_logical, 6);
        memcpy(mp.src_logical, vax2_logical, 6);
        memcpy(mp.peer_logical, vax1_logical, 6);
        mp.remote_conid = 0x33580008;
        mp.local_conid = 0x62C50009;
        mp.recv_ack = 10;
        mp.send_seq = 11;
        mp.incarnation = 1;
        mp.sysap_send_msg = 2;
        mp.sysap_ack_msg = 0;
        mp.votes = vote_configs[i];

        uint8_t frame[SCS_MEMBER_FRAME_LEN];
        CHECK(scs_member_build_params(&mp, frame) == 0, "roundtrip: build params");

        /* The emitted VOTES byte sits at the GROUNDED offset body[22:24]. */
        CHECK(frame[14 + SCS_MEMBER_BODY_OFF + SCS_MEMBER_VOTES_BODYOFF]
                  == (uint8_t)(vote_configs[i] & 0xff),
              "roundtrip: VOTES emitted at body[22:24] (abs 94)");

        struct scs_member_view v;
        CHECK(scs_member_parse(frame, sizeof(frame), &v) == 0,
              "roundtrip: parse our own params frame");
        CHECK(v.has_votes == 1, "roundtrip: params frame carries votes");
        CHECK(v.votes == vote_configs[i],
              "roundtrip: parsed VOTES == advertised VOTES");

        /* And the model consumes exactly that consumed wire value. */
        struct scs_quorum q;
        scs_quorum_init(&q);
        CHECK(scs_quorum_set_member(&q, mp.local_conid, v.votes, 1, 1) == 0,
              "roundtrip: feed consumed votes into the quorum model");
        scs_quorum_recompute(&q);
        CHECK(q.present_votes == vote_configs[i],
              "roundtrip: model present votes == wire VOTES");
    }

    /* A non-params frame (op 0x14 model) must NOT report votes -- guarding
     * against reading a votes value out of a frame that carries none. */
    struct scs_member_params mp;
    memset(&mp, 0, sizeof(mp));
    memcpy(mp.dst_mac, vax1_logical, 6);
    memcpy(mp.src_mac, vax2_logical, 6);
    memcpy(mp.src_logical, vax2_logical, 6);
    memcpy(mp.peer_logical, vax1_logical, 6);
    mp.remote_conid = 0x33580008;
    mp.local_conid = 0x62C50009;
    mp.sysap_send_msg = 1;
    uint8_t frame[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_model(&mp, frame) == 0, "roundtrip: build model");
    struct scs_member_view v;
    CHECK(scs_member_parse(frame, sizeof(frame), &v) == 0, "roundtrip: parse model");
    CHECK(v.has_votes == 0, "roundtrip: op 0x14 model frame reports no votes");
}

int main(void)
{
    test_five_node_worked_example();
    test_three_node_documented_example();
    test_expected_votes_dominates();
    test_sum_votes_dominates();
    test_cevotes_never_decreases();
    test_quorum_disk();
    test_ovmx_nonvoting_contribution();
    test_edge();
    test_votes_wire_roundtrip();

    if (failures == 0) {
        printf("PASS: scs_quorum unit tests (CEVOTES/QUORUM + gate + VOTES wire)\n");
        return 0;
    }
    fprintf(stderr, "%d assertion(s) failed\n", failures);
    return 1;
}
