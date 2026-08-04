/*
 * test_scs_credit.c - SCA message flow control: the Send / Receive / Pending
 * Receive Credit account per connection (vms-76e).
 *
 * Asserts the documented debit/credit system of Roy G. Davis, *VAXcluster
 * Principles*, Digital Press 1993, ch. 2 sec 2.8:
 *   - the CDT carries Send, Receive and Pending Receive Credit for the
 *     connection                                            (pp. 2-43..2-45)
 *   - formation extends N Send Credits by contributing N buffers to the port
 *     MFREEQ; the remote's Send Credit mirrors it                   (p. 2-43)
 *   - every outbound message piggybacks the local Pending Receive Credit in
 *     its header credit field and resets it to 0                    (p. 2-44)
 *   - every inbound message adds its credit field to Send Credit    (p. 2-43)
 *   - THE WORKED EXAMPLE on pp. 2-43..2-44, and its simultaneous-send
 *     variation ending at 9                                         (p. 2-44)
 *   - Credit Wait: no Send Credits, no send                         (p. 2-45)
 *   - dangerously low Receive Credit -> special credit message      (p. 2-44)
 *
 * AND the grounded wire field: the credit field is read out of TWO REAL
 * CAPTURED FRAMES from our own lab captures (see the WIRE VERDICT in
 * scs_credit.h for the conservation proof and the SYSGEN-tunable match).
 *
 * AND the OVMX_NO_CREDIT_ACCOUNTING kill switch, exercised by re-running the
 * whole worked example with the switch set and asserting NOTHING moves.
 *
 * REACHABILITY: a green run here proves the accounting is CORRECT, not that
 * OVMX USES it. scsd.c calls none of this and emits no live credit -- see the
 * reachability note in scs_credit.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scs_credit.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        checks++;                                                                        \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            printf("FAIL %s:%d: ", __func__, __LINE__);                                  \
            printf(__VA_ARGS__);                                                         \
            printf("\n");                                                                \
        }                                                                                \
    } while (0)

/* --- a two-node bench: two mirror-image CDTs, one per side (p. 2-28) ------ */

struct bench {
    struct scs_config cfg_l, cfg_r;
    struct scs_pdt    pdt_l, pdt_r;
    struct scs_cdl    cdl_l, cdl_r;
    struct scs_cdt   *local;  /* the "local SYSAP" half of the p. 2-43 example */
    struct scs_cdt   *remote; /* the "remote SYSAP" half */
};

static const uint8_t mac_l[6]   = {0x08, 0x00, 0x2b, 0x11, 0x11, 0x11};
static const uint8_t mac_r[6]   = {0x08, 0x00, 0x2b, 0x22, 0x22, 0x22};
static const uint8_t sysid_l[6] = {0xaa, 0x00, 0x04, 0x00, 0x01, 0x04};
static const uint8_t sysid_r[6] = {0xaa, 0x00, 0x04, 0x00, 0x02, 0x04};

static struct scs_cdt *make_side(struct scs_config *cfg, struct scs_pdt *pdt,
                                 struct scs_cdl *cdl, const uint8_t mac[6],
                                 const uint8_t sysid[6])
{
    scs_config_init(cfg);
    scs_pdt_init(pdt, SCS_PORT_TYPE_ETHERNET, 4096);
    scs_cdl_init(cdl);

    struct scs_pb *pb = scs_pb_create(cfg, pdt, mac, SCS_PORT_TYPE_ETHERNET);
    if (pb == NULL || scs_pb_learn_system_addr(cfg, pb, sysid) == NULL) {
        return NULL;
    }
    if (scs_pb_open(cfg, pb) == SCS_OPEN_ERROR) {
        return NULL;
    }
    return scs_cdl_alloc(cdl, "VMS$VAXcluster  ", "VMS$VAXcluster  ", pb);
}

static int bench_init(struct bench *b)
{
    memset(b, 0, sizeof(*b));
    b->local = make_side(&b->cfg_l, &b->pdt_l, &b->cdl_l, mac_r, sysid_r);
    b->remote = make_side(&b->cfg_r, &b->pdt_r, &b->cdl_r, mac_l, sysid_l);
    return b->local != NULL && b->remote != NULL;
}

/*
 * Form the connection the way p. 2-43 describes it, from BOTH sides: each SYSAP
 * extends `n` Send Credits to its peer, and each side learns from the peer's
 * CONNECT_REQ/ACCEPT_REQ credit field how many it may itself send.
 */
static void form(struct bench *b, unsigned local_n, unsigned remote_n, unsigned min_send)
{
    scs_credit_extend(b->local, local_n, min_send);
    scs_credit_extend(b->remote, remote_n, min_send);
    scs_credit_grant_from_peer(b->remote, local_n);
    scs_credit_grant_from_peer(b->local, remote_n);
}

/* ========================================================================== */

/*
 * p. 2-45: "SCS on each node maintains Send Credit, Receive Credit, and Pending
 * Receive Credit counts for the connection", in the CDT that describes it.
 * p. 2-43: the extended buffers go into the MFREEQ of the port that supports
 * the connection.
 */
static void test_formation_extends_credits(void)
{
    struct bench b;
    CHECK(bench_init(&b), "bench setup failed");

    CHECK(b.pdt_l.mfreeq_count == 0, "MFREEQ starts empty, got %u", b.pdt_l.mfreeq_count);

    form(&b, 10, 10, 1);

    /* "then the local SYSAP is said to have extended 10 Send Credits to the
     * remote SYSAP" (p. 2-43) -- and the remote's Send Credit mirrors it. */
    CHECK(b.local->extended_credits == 10, "local extended %u, want 10",
          b.local->extended_credits);
    CHECK(b.remote->send_credit == 10, "remote Send Credit %u, want 10",
          b.remote->send_credit);

    /* "local SCS sets the local Receive Credit count to 10" (p. 2-44). */
    CHECK(b.local->receive_credit == 10, "local Receive Credit %u, want 10",
          b.local->receive_credit);
    CHECK(b.local->pending_receive_credit == 0, "local Pending Receive %u, want 0",
          b.local->pending_receive_credit);

    /* The 10 buffers went into the MFREEQ of the port supporting the
     * connection (p. 2-43/2-45). */
    CHECK(b.pdt_l.mfreeq_count == 10, "local MFREEQ %u, want 10", b.pdt_l.mfreeq_count);
    CHECK(b.pdt_r.mfreeq_count == 10, "remote MFREEQ %u, want 10", b.pdt_r.mfreeq_count);
}

/*
 * THE WORKED EXAMPLE, pp. 2-43..2-44, reproduced step for step.
 *
 * "Assume that the remote SYSAP has sent 3 messages to the local SYSAP. Remote
 * SCS now thinks that the remote SYSAP has only 7 Send Credits remaining ...
 * The difference between these two numbers, 3, is referred to as the Pending
 * Receive Credit count ... If the local SYSAP now sends a message to the remote
 * SYSAP, local SCS copies the local Pending Receive Credit count, 3, into the
 * credit field of the message header ... remote SCS adds the contents of the
 * credit field to its Send Credit count, thus restoring that count to 10."
 */
static void test_worked_example_p2_43(void)
{
    struct bench b;
    CHECK(bench_init(&b), "bench setup failed");
    form(&b, 10, 10, 1);

    /* The remote SYSAP sends 3 messages to the local SYSAP. */
    for (int i = 0; i < 3; i++) {
        CHECK(scs_credit_can_send(b.remote), "remote should have credit at msg %d", i);
        int carried = scs_credit_on_send(b.remote);
        CHECK(carried == 0, "remote piggybacked %d, want 0 (it has freed nothing)", carried);
        CHECK(scs_credit_on_recv(b.local, (unsigned)carried) == 0, "local recv failed");
    }

    /* "Remote SCS now thinks that the remote SYSAP has only 7 Send Credits
     * remaining on the connection." (p. 2-43) */
    CHECK(b.remote->send_credit == 7, "remote Send Credit %u, want 7", b.remote->send_credit);

    /* "as a result of all three of those messages having been received by the
     * local node, local SCS has also decremented the local Receive Credit to
     * 7." (p. 2-44) */
    CHECK(b.local->receive_credit == 7, "local Receive Credit %u, want 7",
          b.local->receive_credit);

    /* "what if the local SYSAP has processed the contents of all three of those
     * buffers and released them back to local SCS[?]" (p. 2-43) */
    for (int i = 0; i < 3; i++) {
        CHECK(scs_credit_release_buffer(b.local) == 0, "release %d failed", i);
    }

    /* "The difference between these two numbers, 3, is referred to as the
     * Pending Receive Credit count." (p. 2-43) */
    CHECK(b.local->pending_receive_credit == 3, "local Pending Receive %u, want 3",
          b.local->pending_receive_credit);
    /* And in fact all 10 buffers are free again on the local node (p. 2-43). */
    CHECK(b.pdt_l.mfreeq_count == 10, "local MFREEQ %u, want 10 (all released)",
          b.pdt_l.mfreeq_count);

    /* "If the local SYSAP now sends a message to the remote SYSAP, local SCS
     * copies the local Pending Receive Credit count, 3, into the credit field
     * of the message header. Local SCS also resets to 0 the local Pending
     * Receive Credit." (p. 2-44) */
    int carried = scs_credit_on_send(b.local);
    CHECK(carried == 3, "local piggybacked %d, want 3", carried);
    CHECK(b.local->pending_receive_credit == 0, "local Pending Receive %u after send, want 0",
          b.local->pending_receive_credit);

    /* "When the remote node receives the message, remote SCS adds the contents
     * of the credit field to its Send Credit count, thus restoring that count
     * to 10." (p. 2-44) */
    CHECK(scs_credit_on_recv(b.remote, (unsigned)carried) == 0, "remote recv failed");
    CHECK(b.remote->send_credit == 10, "remote Send Credit %u, want 10 (restored)",
          b.remote->send_credit);

    /* The Receive Credit count is "effectively a mirror image of remote SCS's
     * Send Credit count" (p. 2-44) -- so it is back at 10 too. */
    CHECK(b.local->receive_credit == 10, "local Receive Credit %u, want 10 (mirror)",
          b.local->receive_credit);
}

/*
 * THE VARIATION, p. 2-44: "Suppose that both SYSAPs send messages to each other
 * at the same time. Remote SCS decrements the Send Credit count it associates
 * with the connection from 7 to 6. At the same time, local SCS copies the local
 * Pending Receive Credit count, 3, into the credit field ... remote SCS will add
 * the 3 to the 6, thus associating a Send Credit count of 9 with the
 * connection ... when the local SYSAP releases that buffer back to local SCS,
 * local SCS increments the local Pending Receive Credit count from 0 to 1."
 */
static void test_worked_example_variation_p2_44(void)
{
    struct bench b;
    CHECK(bench_init(&b), "bench setup failed");
    form(&b, 10, 10, 1);

    /* Same starting position as the first example: 3 sent, 3 released. */
    for (int i = 0; i < 3; i++) {
        int c = scs_credit_on_send(b.remote);
        scs_credit_on_recv(b.local, (unsigned)c);
        scs_credit_release_buffer(b.local);
    }
    CHECK(b.remote->send_credit == 7, "precondition: remote Send Credit %u, want 7",
          b.remote->send_credit);
    CHECK(b.local->pending_receive_credit == 3, "precondition: local Pending Receive %u, want 3",
          b.local->pending_receive_credit);

    /* Simultaneous sends: both sides debit before either message arrives. */
    int remote_carried = scs_credit_on_send(b.remote);
    CHECK(b.remote->send_credit == 6, "remote Send Credit %u, want 6", b.remote->send_credit);
    int local_carried = scs_credit_on_send(b.local);
    CHECK(local_carried == 3, "local piggybacked %d, want 3", local_carried);
    CHECK(b.local->pending_receive_credit == 0, "local Pending Receive %u, want 0",
          b.local->pending_receive_credit);

    /* The remote receives the local message: 6 + 3 = 9. */
    scs_credit_on_recv(b.remote, (unsigned)local_carried);
    CHECK(b.remote->send_credit == 9, "remote Send Credit %u, want 9", b.remote->send_credit);

    /* The local receives the remote message and later releases its buffer:
     * "local SCS increments the local Pending Receive Credit count from 0 to 1." */
    scs_credit_on_recv(b.local, (unsigned)remote_carried);
    CHECK(b.local->pending_receive_credit == 0, "still 0 before the SYSAP releases, got %u",
          b.local->pending_receive_credit);
    scs_credit_release_buffer(b.local);
    CHECK(b.local->pending_receive_credit == 1, "local Pending Receive %u, want 1",
          b.local->pending_receive_credit);
}

/*
 * p. 2-45 Credit Wait: "this routine first verifies that at least one Send
 * Credit is available on the connection being used. If no Send Credits are
 * available, then this routine temporarily suspends the operation."
 *
 * OVMX has no CDRP to queue, so it REFUSES the send instead of faking one.
 */
static void test_credit_wait_p2_45(void)
{
    struct bench b;
    CHECK(bench_init(&b), "bench setup failed");
    form(&b, 10, 3, 1); /* the remote extended only 3 credits to us */

    CHECK(b.local->send_credit == 3, "local Send Credit %u, want 3", b.local->send_credit);
    for (int i = 0; i < 3; i++) {
        CHECK(scs_credit_on_send(b.local) >= 0, "send %d should succeed", i);
    }
    CHECK(b.local->send_credit == 0, "local Send Credit %u, want 0", b.local->send_credit);

    /* The fourth send goes into a Credit Wait: refused, and NOTHING changed. */
    CHECK(scs_credit_can_send(b.local) == 0, "can_send should be 0 at zero credit");
    unsigned pending_before = b.local->pending_receive_credit;
    unsigned recv_before = b.local->receive_credit;
    CHECK(scs_credit_on_send(b.local) == -1, "send at zero credit must refuse");
    CHECK(b.local->send_credit == 0, "Send Credit moved on a refused send: %u",
          b.local->send_credit);
    CHECK(b.local->pending_receive_credit == pending_before,
          "Pending Receive moved on a refused send: %u", b.local->pending_receive_credit);
    CHECK(b.local->receive_credit == recv_before, "Receive Credit moved on a refused send: %u",
          b.local->receive_credit);

    /* "Whenever the Send Credit count in a CDT is increased, the CDT's queue of
     * waiting CDRPs is examined" (p. 2-45) -- an inbound credit unblocks it. */
    scs_credit_on_recv(b.local, 2);
    CHECK(b.local->send_credit == 2, "Send Credit %u after a 2-credit message, want 2",
          b.local->send_credit);
    CHECK(scs_credit_can_send(b.local) == 1, "should be sendable again");
}

/*
 * p. 2-44 special credit message. "SCA defines the local Receive Credit count
 * for a connection to be dangerously low if it is less than the Minimum Send
 * Credits argument passed to the CONNECT or ACCEPT service by the remote SYSAP.
 * The VMS implementation ... if it is less than the sum of the local SYSGEN
 * parameter SCSFLOWCUSH and the remote value for Minimum Send Credits."
 */
static void test_special_credit_message_p2_44(void)
{
    struct bench b;
    CHECK(bench_init(&b), "bench setup failed");

    const unsigned min_send = 4;
    const unsigned threshold = (unsigned)SCS_CREDIT_SCSFLOWCUSH + min_send;
    form(&b, 10, 10, min_send);

    CHECK(!scs_credit_is_dangerously_low(b.local), "10 credits is not dangerously low");
    CHECK(scs_credit_take_special(b.local) == -1, "no special message when not low");

    /* One-way traffic: the remote sends, the local releases, and never sends
     * back -- so nothing is piggybacked and Receive Credit falls.
     *
     * BOUNDED ON PURPOSE. Receive Credit starts at the 10 extended credits and
     * must fall by exactly 1 per message, so it crosses below threshold in 5
     * steps and can never need more than 10. The guard makes a receive path
     * that fails to debit Receive Credit FAIL HERE with a diagnosis instead of
     * spinning forever -- an unbounded loop turns that mutant into a CMake
     * TIMEOUT, which is a hang, not a test result. */
    const unsigned max_sends = 10;
    unsigned sent = 0;
    unsigned saw_boundary = 0;
    while (b.local->receive_credit >= threshold && sent < max_sends) {
        /*
         * THE OFF-BY-ONE BOUNDARY, asserted because nothing else does.
         * p. 2-44 says dangerously low is "LESS THAN the sum of ... SCSFLOWCUSH
         * and the remote value for Minimum Send Credits" -- so Receive Credit
         * EQUAL to that sum is NOT low. Without this, the test only ever
         * observes Receive Credit at 10 and 5 and `<` vs `<=` in
         * scs_credit_is_dangerously_low() is unobservable: a mutant that
         * reports the boundary as low passes the whole suite. Receive Credit
         * falls 10,9,8,7,6 through this loop, so `== threshold` is reached on
         * the last iteration; saw_boundary below proves it was.
         */
        if (b.local->receive_credit == threshold) {
            CHECK(!scs_credit_is_dangerously_low(b.local),
                  "Receive Credit == threshold (%u) must NOT be dangerously low: "
                  "p. 2-44 says \"less than the sum\", not \"at most\"",
                  threshold);
            saw_boundary = 1;
        }
        int c = scs_credit_on_send(b.remote);
        CHECK(c >= 0, "remote send %u refused", sent);
        scs_credit_on_recv(b.local, (unsigned)c);
        scs_credit_release_buffer(b.local);
        sent++;
    }
    CHECK(sent < max_sends,
          "Receive Credit still %u after %u one-way messages -- the receive path "
          "is not debiting Receive Credit",
          b.local->receive_credit, sent);
    CHECK(saw_boundary,
          "Receive Credit never passed through exactly %u -- the boundary "
          "assertion above did not run, so `<` vs `<=` is still untested",
          threshold);
    CHECK(b.local->receive_credit == threshold - 1, "Receive Credit %u, want %u",
          b.local->receive_credit, threshold - 1);
    /* ...and one below the sum IS low. Together with the == assertion in the
     * loop this brackets the p. 2-44 comparison exactly. */
    CHECK(scs_credit_is_dangerously_low(b.local), "should be dangerously low at %u < %u",
          b.local->receive_credit, threshold);
    CHECK(b.local->pending_receive_credit == sent, "Pending Receive %u, want %u",
          b.local->pending_receive_credit, sent);

    /* "local SCS immediately sends remote SCS a special credit message
     * containing the local Pending Receive Credit count ... [and] also resets
     * the local Pending Receive Credit count to 0". */
    int special = scs_credit_take_special(b.local);
    CHECK(special == (int)sent, "special credit message carries %d, want %u", special, sent);
    CHECK(b.local->pending_receive_credit == 0, "Pending Receive %u after special, want 0",
          b.local->pending_receive_credit);
    CHECK(!scs_credit_is_dangerously_low(b.local), "no longer low after the special message");

    /* The remote's Send Credit is restored by exactly that amount. */
    unsigned before = b.remote->send_credit;
    scs_credit_on_recv(b.remote, (unsigned)special);
    CHECK(b.remote->send_credit == before + sent, "remote Send Credit %u, want %u",
          b.remote->send_credit, before + sent);
}

/* --- the grounded wire field --------------------------------------------- */

/*
 * REAL CAPTURED FRAMES, /data/training/vax/cluster/captures/
 * formation-ci1-joinwindow.pcap. SCA content only (absolute frame offset 14
 * onward); no VSI/HPE source or binary was read (CLAUDE.md rule 8).
 *
 * raw frame 47: the 110-byte VMS$VAXcluster CONNECT-REQUEST (remote Con.ID 0,
 * local 0x62C50009 -- the same frame scs_connect.c uses as its template).
 * Its credit field carries 10 = SYSGEN CLUSTER_CREDITS: the Send Credits the
 * VMS$VAXcluster SYSAP is extending at connection formation (p. 2-43).
 */
static const uint8_t frame110_connect_req[110] = {
    0x6C, 0x00, 0xAA, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 0x00, 0xAA, 0x00,
    0x04, 0x00, 0x01, 0x04, 0x4B, 0x13, 0x06, 0x00, 0x07, 0x00, 0x01, 0x00,
    0x12, 0x00, 0x06, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x06, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x42, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0xC5, 0x62, 0x00, 0x00,
    0x01, 0x00, 0x56, 0x4D, 0x53, 0x24, 0x56, 0x41, 0x58, 0x63, 0x6C, 0x75,
    0x73, 0x74, 0x65, 0x72, 0x20, 0x20, 0x56, 0x4D, 0x53, 0x24, 0x56, 0x41,
    0x58, 0x63, 0x6C, 0x75, 0x73, 0x74, 0x65, 0x72, 0x20, 0x20, 0x01, 0x1B,
    0x01, 0x03, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x08, 0x00, 0x00,
    0x06, 0x00
};

/*
 * raw frame 68: a 190-byte steady-state sequenced message on the bound Con.ID
 * pair (0x33580008 / 0x62C50009). Its credit field carries 3 -- a piggybacked
 * Pending Receive Credit, exactly the p. 2-44 mechanism.
 */
static const uint8_t frame190_vc[190] = {
    0xBC, 0x00, 0xAA, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 0x00, 0xAA, 0x00,
    0x04, 0x00, 0x01, 0x04, 0x4B, 0x13, 0x10, 0x00, 0x10, 0x00, 0x01, 0x00,
    0x12, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x92, 0x00, 0x04, 0x00, 0x0A, 0x00,
    0x03, 0x00, 0x08, 0x00, 0x58, 0x33, 0x09, 0x00, 0xC5, 0x62, 0x03, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static void test_wire_credit_field(void)
{
    uint16_t v = 0xFFFF;

    /* Sanity: these arrays really are the frames they claim to be -- SCA length
     * word, opcode and format constant, so a bad paste cannot pass silently. */
    CHECK((uint16_t)(frame110_connect_req[0] | (frame110_connect_req[1] << 8)) + 2 == 110,
          "110-frame SCA length word is wrong");
    CHECK(frame110_connect_req[16] == 0x4B && frame110_connect_req[17] == 0x13,
          "110-frame is not a 0x4b/0x13 SCS message");
    CHECK((uint16_t)(frame190_vc[0] | (frame190_vc[1] << 8)) + 2 == 190,
          "190-frame SCA length word is wrong");
    CHECK(frame190_vc[16] == 0x4B && frame190_vc[17] == 0x13,
          "190-frame is not a 0x4b/0x13 SCS message");

    /* Formation: the CONNECT-REQUEST extends 10 = SYSGEN CLUSTER_CREDITS. */
    CHECK(scs_credit_read_header(frame110_connect_req, sizeof(frame110_connect_req), &v) == 0,
          "reading the 110-byte class failed");
    CHECK(v == 10, "110-byte CONNECT-REQUEST credit field = %u, want 10 (CLUSTER_CREDITS)", v);

    /* Steady state: a piggybacked Pending Receive Credit of 3. */
    CHECK(scs_credit_read_header(frame190_vc, sizeof(frame190_vc), &v) == 0,
          "reading the 190-byte class failed");
    CHECK(v == 3, "190-byte VC message credit field = %u, want 3", v);

    /* The field sits immediately before the destination Con.ID at SCA [50:54]. */
    CHECK(scs_credit_header_offset(190) == 48, "190-byte credit offset is not 48");
    CHECK(SCS_CREDIT_FIELD_SCA_OFFSET + SCS_CREDIT_FIELD_LEN == 50,
          "the credit field must abut the Con.ID pair at SCA 50");

    /* Ungrounded classes are REFUSED, not guessed: the 41-byte 0x48 short does
     * not even reach offset 48, and the block-data-transfer classes carry
     * something else there entirely. */
    CHECK(scs_credit_header_offset(41) == -1, "the 41-byte 0x48 short must be refused");
    CHECK(scs_credit_header_offset(526) == -1, "the 526-byte block class must be refused");
    CHECK(scs_credit_read_header(frame190_vc, 41, &v) == -1, "41-byte read must be refused");

    /*
     * THE ADMITTED SET IS EXACTLY THE MEASURED SET -- nothing extrapolated.
     * Each of these seven was tabulated over all 47 captures under the 0x4B13
     * filter and observed credit-shaped at offset 48 (per-class n / distinct /
     * max in the WIRE VERDICT in scs_credit.h). This loop pins the set closed:
     * any length not listed must be refused.
     */
    static const uint16_t grounded[] = {58, 62, 66, 86, 94, 110, 190};
    for (size_t i = 0; i < sizeof(grounded) / sizeof(grounded[0]); i++) {
        CHECK(scs_credit_header_offset(grounded[i]) == SCS_CREDIT_FIELD_SCA_OFFSET,
              "grounded class %u must map to offset %d", grounded[i],
              SCS_CREDIT_FIELD_SCA_OFFSET);
    }
    for (uint32_t len = 0; len <= 0xFFFFu; len++) {
        int want = -1;
        for (size_t i = 0; i < sizeof(grounded) / sizeof(grounded[0]); i++) {
            if (grounded[i] == (uint16_t)len) {
                want = SCS_CREDIT_FIELD_SCA_OFFSET;
            }
        }
        if (scs_credit_header_offset((uint16_t)len) != want) {
            CHECK(0, "class %u: offset %d, want %d -- the admitted set drifted", len,
                  scs_credit_header_offset((uint16_t)len), want);
            break;
        }
    }

    /*
     * 106 IN PARTICULAR (vms-76e, adversary-caught regression guard). An earlier
     * revision admitted 106 as a grounded SCS class. It is not one: across all
     * 47 pcaps in /data/training/vax/cluster/captures/ there are ZERO 106-byte
     * SCA frames carrying the 0x4B13 SCS marker, and all 792 that do exist are
     * marker 0x4113 -- the START/config class of cluster-protocol-spec sec 4(j),
     * which has no credit field (sca[48:50] is a constant 0 there, 792/792).
     */
    CHECK(scs_credit_header_offset(106) == -1,
          "the 106-byte class is 0x4113 START, not an SCS message -- must be refused");

    /* Stamping is the exact inverse of reading, on a real frame. */
    uint8_t out[190];
    memcpy(out, frame190_vc, sizeof(out));
    CHECK(scs_credit_stamp_header(out, sizeof(out), 7) == 0, "stamp failed");
    CHECK(scs_credit_read_header(out, sizeof(out), &v) == 0 && v == 7, "stamp/read round trip");
    /* ...and it touches ONLY those two bytes. */
    out[48] = frame190_vc[48];
    out[49] = frame190_vc[49];
    CHECK(memcmp(out, frame190_vc, sizeof(out)) == 0,
          "stamping changed a byte outside the credit field");
}

/* ==========================================================================
 * vms-1d2: the CREDIT WAIT QUEUE (pp. 2-45..2-46) and the SPECIAL CREDIT
 * MESSAGE TRIGGER (p. 2-44).
 *
 * vms-76e proved the credit TEST. These prove the mechanism the test gates:
 * a starved sender is SUSPENDED on the CDT rather than refused, and is resumed
 * FIFO by the arrival of credit.
 *
 * THE EMIT PROXY, and why it is not a fiction. OVMX has no SYSAP that sends an
 * SCS message, so "did the starved sender emit?" is asserted against a stand-in
 * sender: a resumed operation stamps a REAL captured 190-byte VC frame
 * (frame190_vc, the same specimen the wire test reads) through the REAL
 * production stamper, scs_credit_stamp_header(). The frame's credit field is
 * then read back with scs_credit_read_header(). So "did not emit" means the
 * frame buffer is still untouched, and "emitted carrying N" means the grounded
 * SCA [48:50] field of a real frame holds N. What is NOT proven is that any
 * OVMX daemon ever runs this: nothing in scsd.c calls the credit module at all
 * (see the reachability note in scs_credit.h). These senders are the test's,
 * not production's.
 * ========================================================================== */

#define MAX_EMITS 16

struct sender {
    struct scs_credit_waiter w;
    int      id;
    uint8_t  frame[190]; /* zeroed = nothing was ever emitted */
    int      emits;
    unsigned carried; /* credit value of the last frame it emitted */

    /* A SYSAP with more to send: when this one is resumed it immediately issues
     * a second send, for `chain`. That is the only way to observe a send made
     * while Send Credit is > 0 AND operations are still queued -- the state the
     * fast path in scs_credit_send_or_wait() has to refuse. `chain_rc` records
     * what that nested send_or_wait returned. NULL for every other test. */
    struct sender *chain;
    int            chain_rc;
    int            chain_done;
};

static int g_emit_order[MAX_EMITS];
static int g_emit_n;

static void emit_reset(void)
{
    g_emit_n = 0;
    memset(g_emit_order, 0, sizeof(g_emit_order));
}

/* The stand-in SYSAP send: build the message and put the piggyback credit in
 * its header (p. 2-44), using the production stamper on a real captured frame. */
static void sender_emit(struct sender *s, unsigned credit)
{
    memcpy(s->frame, frame190_vc, sizeof(s->frame));
    if (scs_credit_stamp_header(s->frame, sizeof(s->frame), credit) != 0) {
        CHECK(0, "sender %d: stamping the credit field failed", s->id);
    }
    s->carried = credit;
    s->emits++;
    if (g_emit_n < MAX_EMITS) {
        g_emit_order[g_emit_n++] = s->id;
    }
}

/* p. 2-46: "the address of the instruction at which to resume the operation is
 * kept in the CDRP" -- here, a callback on the waiter. */
static void sender_resume(struct scs_credit_waiter *w, unsigned credit, void *ctx)
{
    struct sender *s = (struct sender *)ctx;
    CHECK(w == &s->w, "resume delivered the wrong waiter to sender %d", s->id);
    sender_emit(s, credit);

    /* The resumed SYSAP has more to send. This runs INSIDE the drain, while
     * later waiters are still queued and the grant is not yet exhausted. */
    if (s->chain != NULL && !s->chain_done) {
        s->chain_done = 1;
        s->chain_rc = scs_credit_send_or_wait(w->cdt, &s->chain->w);
    }
}

static void sender_init(struct sender *s, int id)
{
    memset(s, 0, sizeof(*s));
    s->id = id;
    s->w.resume = sender_resume;
    s->w.ctx = s;
}

/* Did this sender's frame stay untouched? */
static int sender_silent(const struct sender *s)
{
    for (size_t i = 0; i < sizeof(s->frame); i++) {
        if (s->frame[i] != 0) {
            return 0;
        }
    }
    return s->emits == 0;
}

/* Read the credit field back out of what the sender actually emitted. */
static int sender_frame_credit(const struct sender *s, uint16_t *out)
{
    return scs_credit_read_header(s->frame, sizeof(s->frame), out);
}

/*
 * The special credit message emitter (p. 2-44). Records every firing: how many,
 * what count each carried, and the Receive Credit at the moment it fired.
 */
struct special_rec {
    int      fires;
    unsigned credit[MAX_EMITS];
    unsigned receive_credit_at_fire[MAX_EMITS];
};

static void special_recorder(struct scs_cdt *cdt, unsigned credit, void *ctx)
{
    struct special_rec *r = (struct special_rec *)ctx;
    if (r->fires < MAX_EMITS) {
        r->credit[r->fires] = credit;
        r->receive_credit_at_fire[r->fires] = cdt->receive_credit;
    }
    r->fires++;
}

/*
 * p. 2-45: "If no Send Credits are available, then this routine temporarily
 * suspends the operation involved by placing it in a Credit Wait. This is done
 * by queuing the CDRP representing the operation to the CDT for the
 * connection."
 *
 * A send with 0 credits BLOCKS -- it is queued to the CDT -- and emits nothing.
 */
static void test_credit_wait_blocks_and_does_not_emit(void)
{
    struct bench b;
    CHECK(bench_init(&b), "bench setup failed");
    form(&b, 10, 2, 1); /* the remote extended only 2 credits to us */
    emit_reset();

    struct sender s1, s2, s3;
    sender_init(&s1, 1);
    sender_init(&s2, 2);
    sender_init(&s3, 3);

    /* The first two sends have credit: the fast path returns the piggyback
     * value and the operation is NOT queued -- it sends on its own stack. */
    int r = scs_credit_send_or_wait(b.local, &s1.w);
    CHECK(r >= 0, "send 1 should have credit, got %d", r);
    sender_emit(&s1, (unsigned)r);
    CHECK(s1.w.queued == 0, "a send that had credit must not be queued");
    CHECK(scs_credit_wait_depth(b.local) == 0, "queue depth %u after an unblocked send",
          scs_credit_wait_depth(b.local));

    r = scs_credit_send_or_wait(b.local, &s2.w);
    CHECK(r >= 0, "send 2 should have credit, got %d", r);
    sender_emit(&s2, (unsigned)r);
    CHECK(b.local->send_credit == 0, "Send Credit %u after 2 sends, want 0",
          b.local->send_credit);

    /* The third send is starved. Snapshot everything the account holds. */
    unsigned send_before = b.local->send_credit;
    unsigned recv_before = b.local->receive_credit;
    unsigned pend_before = b.local->pending_receive_credit;
    unsigned mfreeq_before = b.pdt_l.mfreeq_count;

    r = scs_credit_send_or_wait(b.local, &s3.w);
    CHECK(r == SCS_CREDIT_WAIT, "starved send must report SCS_CREDIT_WAIT, got %d", r);

    /* It BLOCKED: queued to the CDT (p. 2-45), not refused and not sent. */
    CHECK(scs_credit_wait_depth(b.local) == 1, "queue depth %u, want 1",
          scs_credit_wait_depth(b.local));
    CHECK(s3.w.queued == 1, "the starved waiter is not marked queued");
    CHECK(s3.w.resumed == 0, "the starved waiter must not be resumed yet");
    CHECK(b.local->credit_wait_head == &s3.w, "the CDT's queue head is not the waiter");

    /* AND IT DID NOT EMIT. */
    CHECK(sender_silent(&s3), "a blocked sender emitted a frame (%d emits)", s3.emits);

    /* Nothing in the account moved. */
    CHECK(b.local->send_credit == send_before, "Send Credit moved while blocking: %u",
          b.local->send_credit);
    CHECK(b.local->receive_credit == recv_before, "Receive Credit moved while blocking: %u",
          b.local->receive_credit);
    CHECK(b.local->pending_receive_credit == pend_before,
          "Pending Receive moved while blocking: %u", b.local->pending_receive_credit);
    CHECK(b.pdt_l.mfreeq_count == mfreeq_before, "MFREEQ moved while blocking: %u",
          b.pdt_l.mfreeq_count);

    /* A second queue attempt with the same waiter is refused, not double-linked. */
    CHECK(scs_credit_send_or_wait(b.local, &s3.w) == -1,
          "re-queuing an already-queued waiter must be refused");
    CHECK(scs_credit_wait_depth(b.local) == 1, "depth %u after a refused re-queue",
          scs_credit_wait_depth(b.local));

    CHECK(scs_credit_wait_flush(b.local) == 1, "flush should have dropped 1 waiter");
    CHECK(scs_credit_wait_depth(b.local) == 0, "flush left the queue non-empty");
}

/*
 * p. 2-45: "Whenever the Send Credit count in a CDT is increased, the CDT's
 * queue of waiting CDRPs is examined. If that queue is nonempty, as many
 * waiting CDRPs as possible are resumed, based on the number of Send Credits
 * currently available."
 * p. 2-46: "Each of the SCS wait queues described here is a 'first in first
 * out' queue ... the CDRP at the head of the queue has priority."
 *
 * THE TRIGGER IS DRIVEN, NOT SIMULATED: the queue is released by
 * scs_credit_on_recv() -- an inbound message carrying a credit field -- which
 * is the ordinary production path by which Send Credit rises. This test never
 * calls scs_credit_wait_release() itself.
 */
static void test_credit_wait_released_fifo(void)
{
    struct bench b;
    CHECK(bench_init(&b), "bench setup failed");
    form(&b, 10, 0, 1); /* the remote extended 0 credits: we are starved at once */
    emit_reset();

    CHECK(b.local->send_credit == 0, "local Send Credit %u, want 0", b.local->send_credit);

    /* The local SYSAP has freed 4 received buffers that the remote does not know
     * about yet (p. 2-43), so the first resumed send has 4 to piggyback. */
    for (int i = 0; i < 4; i++) {
        CHECK(scs_credit_release_buffer(b.local) == 0, "release %d failed", i);
    }
    CHECK(b.local->pending_receive_credit == 4, "Pending Receive %u, want 4",
          b.local->pending_receive_credit);

    struct sender s1, s2, s3;
    sender_init(&s1, 1);
    sender_init(&s2, 2);
    sender_init(&s3, 3);
    CHECK(scs_credit_send_or_wait(b.local, &s1.w) == SCS_CREDIT_WAIT, "s1 should block");
    CHECK(scs_credit_send_or_wait(b.local, &s2.w) == SCS_CREDIT_WAIT, "s2 should block");
    CHECK(scs_credit_send_or_wait(b.local, &s3.w) == SCS_CREDIT_WAIT, "s3 should block");
    CHECK(scs_credit_wait_depth(b.local) == 3, "queue depth %u, want 3",
          scs_credit_wait_depth(b.local));
    CHECK(g_emit_n == 0, "%d frames emitted while all three were blocked", g_emit_n);

    unsigned recv_before = b.local->receive_credit;

    /* A message arrives carrying 2 credits: Send Credit rises by 2, so exactly
     * TWO waiters may go -- "as many as possible, based on the number of Send
     * Credits currently available" (p. 2-45). */
    CHECK(scs_credit_on_recv(b.local, 2) == 0, "recv failed");

    CHECK(g_emit_n == 2, "%d senders resumed on a 2-credit grant, want 2", g_emit_n);
    CHECK(g_emit_order[0] == 1 && g_emit_order[1] == 2,
          "resume order was %d,%d -- p. 2-46 requires FIFO 1,2", g_emit_order[0],
          g_emit_order[1]);
    CHECK(scs_credit_wait_depth(b.local) == 1, "queue depth %u after 2 resumes, want 1",
          scs_credit_wait_depth(b.local));
    CHECK(sender_silent(&s3), "s3 was resumed out of turn");
    CHECK(b.local->send_credit == 0, "Send Credit %u after 2 resumes, want 0",
          b.local->send_credit);

    /* The head of the queue carried the whole outstanding Pending Receive Credit
     * (p. 2-44) and the next carried none -- exactly a run of on_send calls.
     * Read back out of the frame the sender actually built. */
    uint16_t v = 0xFFFF;
    CHECK(sender_frame_credit(&s1, &v) == 0 && v == 4,
          "s1's emitted frame carries credit %u, want 4", v);
    CHECK(sender_frame_credit(&s2, &v) == 0 && v == 0,
          "s2's emitted frame carries credit %u, want 0", v);
    CHECK(b.local->pending_receive_credit == 0, "Pending Receive %u after the grant, want 0",
          b.local->pending_receive_credit);
    /* -1 for the message that arrived, +4 mirrored from the piggyback (p. 2-44). */
    CHECK(b.local->receive_credit == recv_before - 1 + 4, "Receive Credit %u, want %u",
          b.local->receive_credit, recv_before - 1 + 4);

    /* One more credit releases the last waiter, still FIFO. */
    CHECK(scs_credit_on_recv(b.local, 1) == 0, "recv failed");
    CHECK(g_emit_n == 3 && g_emit_order[2] == 3, "the third resume was %d, want sender 3",
          g_emit_n == 3 ? g_emit_order[2] : -1);
    CHECK(scs_credit_wait_depth(b.local) == 0, "queue depth %u, want 0",
          scs_credit_wait_depth(b.local));
    CHECK(s3.w.resumed == 1 && s3.emits == 1, "s3 did not emit exactly once");
    CHECK(sender_frame_credit(&s3, &v) == 0 && v == 0, "s3 carried %u, want 0", v);

    /* A credit that arrives with an empty queue resumes nobody. */
    CHECK(scs_credit_on_recv(b.local, 1) == 0, "recv failed");
    CHECK(g_emit_n == 3, "a grant on an empty queue emitted %d frames", g_emit_n);
}

/*
 * A newly arriving sender must not overtake operations already waiting
 * (p. 2-46: "Queue priority is based on time spent in the queue"), and a
 * cancelled operation leaves the FIFO order of the rest intact.
 */
static void test_credit_wait_order_is_preserved(void)
{
    struct bench b;
    CHECK(bench_init(&b), "bench setup failed");
    form(&b, 10, 0, 1);
    emit_reset();

    struct sender s1, s2, s3, s4;
    sender_init(&s1, 1);
    sender_init(&s2, 2);
    sender_init(&s3, 3);
    sender_init(&s4, 4);
    CHECK(scs_credit_send_or_wait(b.local, &s1.w) == SCS_CREDIT_WAIT, "s1 should block");
    CHECK(scs_credit_send_or_wait(b.local, &s2.w) == SCS_CREDIT_WAIT, "s2 should block");
    CHECK(scs_credit_send_or_wait(b.local, &s3.w) == SCS_CREDIT_WAIT, "s3 should block");

    /* Cancel the middle one: it is dequeued, resumes nothing, emits nothing. */
    CHECK(scs_credit_wait_cancel(b.local, &s2.w) == 0, "cancel failed");
    CHECK(scs_credit_wait_depth(b.local) == 2, "depth %u after a cancel, want 2",
          scs_credit_wait_depth(b.local));
    CHECK(scs_credit_wait_cancel(b.local, &s2.w) == -1, "cancelling twice must be refused");
    CHECK(scs_credit_wait_cancel(b.local, &s4.w) == -1,
          "cancelling a waiter that was never queued must be refused");

    /* s4 arrives now. It goes to the TAIL, behind s1 and s3. */
    CHECK(scs_credit_send_or_wait(b.local, &s4.w) == SCS_CREDIT_WAIT, "s4 should block");
    CHECK(scs_credit_wait_depth(b.local) == 3, "depth %u, want 3",
          scs_credit_wait_depth(b.local));

    /* Grant everything at once. */
    CHECK(scs_credit_on_recv(b.local, 3) == 0, "recv failed");
    CHECK(g_emit_n == 3, "%d resumed, want 3", g_emit_n);
    CHECK(g_emit_order[0] == 1 && g_emit_order[1] == 3 && g_emit_order[2] == 4,
          "resume order %d,%d,%d -- want 1,3,4 (s2 cancelled, s4 last in)",
          g_emit_order[0], g_emit_order[1], g_emit_order[2]);
    CHECK(sender_silent(&s2), "a cancelled operation was resumed anyway");
}

/*
 * p. 2-46: "Each of the SCS wait queues described here is a 'first in first
 * out' queue ... Queue priority is based on time spent in the queue. When a
 * resource associated with the queue (e.g., Send Credit ...) becomes
 * available, the CDRP at the head of the queue has priority for receiving
 * that resource."
 *
 * THE LATE SENDER. A send issued while Send Credit is > 0 but operations are
 * ALREADY SUSPENDED must not consume that credit: it has spent no time in the
 * queue, so the resource belongs to the head. Both witnesses below need the
 * account to hold credit while the queue is non-empty, which happens exactly
 * when a resumed operation issues another send:
 *   (a) NESTED -- inside the drain, with the grant not yet exhausted and later
 *       waiters still queued;
 *   (b) TOP LEVEL -- after the drain, because the pass resumes only the
 *       waiters that were queued when the count rose (the budget is fixed at
 *       that moment), so leftover credit and a leftover waiter coexist.
 */
static void test_credit_wait_late_sender_does_not_overtake(void)
{
    struct bench b;
    CHECK(bench_init(&b), "bench setup failed");
    form(&b, 10, 0, 1); /* the remote extended 0: we are starved at once */
    emit_reset();

    struct sender s1, s2, s3, s4, s5;
    sender_init(&s1, 1);
    sender_init(&s2, 2);
    sender_init(&s3, 3);
    sender_init(&s4, 4);
    sender_init(&s5, 5);

    /* s1's SYSAP has a second message ready the moment it is resumed. */
    s1.chain = &s4;

    CHECK(scs_credit_send_or_wait(b.local, &s1.w) == SCS_CREDIT_WAIT, "s1 should block");
    CHECK(scs_credit_send_or_wait(b.local, &s2.w) == SCS_CREDIT_WAIT, "s2 should block");
    CHECK(scs_credit_send_or_wait(b.local, &s3.w) == SCS_CREDIT_WAIT, "s3 should block");
    CHECK(scs_credit_wait_depth(b.local) == 3, "queue depth %u, want 3",
          scs_credit_wait_depth(b.local));

    /* FIVE credits arrive but only three operations are waiting, so the pass
     * resumes three and two Send Credits survive it. */
    CHECK(scs_credit_on_recv(b.local, 5) == 0, "recv failed");

    /* (a) THE NESTED LATE SENDER. When s1's follow-on send was issued the
     * account held 4 Send Credits and s2/s3 were still queued. */
    CHECK(s1.chain_done == 1, "s1's follow-on send never ran -- the witness is missing");
    CHECK(s1.chain_rc == SCS_CREDIT_WAIT,
          "a send issued while s2/s3 were still suspended returned %d: it took the "
          "fast path and overtook them. p. 2-46 gives the resource to the head",
          s1.chain_rc);
    CHECK(s4.w.queued == 1, "the late sender was not queued");

    CHECK(g_emit_n == 3, "%d senders emitted, want 3 (s4 arrived after the pass began)",
          g_emit_n);
    CHECK(g_emit_order[0] == 1 && g_emit_order[1] == 2 && g_emit_order[2] == 3,
          "resume order %d,%d,%d -- want 1,2,3", g_emit_order[0], g_emit_order[1],
          g_emit_order[2]);
    CHECK(sender_silent(&s4), "the late sender was served in the same pass");
    CHECK(scs_credit_wait_depth(b.local) == 1, "queue depth %u after the pass, want 1",
          scs_credit_wait_depth(b.local));

    /* (b) THE TOP-LEVEL LATE SENDER. The account really does have credit here
     * -- asserted, so the refusal below cannot be vacuous -- and the queue is
     * non-empty, which is the only state that distinguishes the two orderings. */
    CHECK(b.local->send_credit == 2, "Send Credit %u after the pass, want 2",
          b.local->send_credit);
    CHECK(scs_credit_can_send(b.local) == 1,
          "the account has no credit here -- the overtake test would be vacuous");
    CHECK(b.local->credit_wait_head == &s4.w, "the queue head is not the waiting s4");

    unsigned send_before = b.local->send_credit;
    CHECK(scs_credit_send_or_wait(b.local, &s5.w) == SCS_CREDIT_WAIT,
          "a send with 2 Send Credits available but s4 still queued must go to the "
          "TAIL, not take the credit");
    CHECK(b.local->send_credit == send_before,
          "the late sender debited a Send Credit: %u, want %u", b.local->send_credit,
          send_before);
    CHECK(sender_silent(&s5), "the late sender emitted a frame");
    CHECK(scs_credit_wait_depth(b.local) == 2, "queue depth %u, want 2",
          scs_credit_wait_depth(b.local));
    CHECK(b.local->credit_wait_head == &s4.w, "the late sender displaced the head");

    /* And they go in the order they queued. */
    CHECK(scs_credit_on_recv(b.local, 1) == 0, "recv failed");
    CHECK(g_emit_n == 5, "%d emitted after the second grant, want 5", g_emit_n);
    CHECK(g_emit_order[3] == 4 && g_emit_order[4] == 5,
          "the two late senders went %d,%d -- want 4,5 (s4 queued first)",
          g_emit_order[3], g_emit_order[4]);
    CHECK(scs_credit_wait_depth(b.local) == 0, "queue depth %u, want 0",
          scs_credit_wait_depth(b.local));
}

/*
 * CANCELLING AT EVERY POSITION. test_credit_wait_order_is_preserved cancels the
 * MIDDLE of the queue, which never exercises the tail (or head) fix-up.
 *
 * The tail case is the dangerous one: if the tail pointer is left aiming at the
 * dequeued node, the NEXT operation to suspend is linked onto that node instead
 * of onto the queue. It is then unreachable from the head -- credit_wait_depth
 * counts it, but no drain can ever reach it and the operation is never resumed.
 * p. 2-45 promises that "as many waiting CDRPs as possible are resumed"; a
 * waiter that cannot be reached breaks that promise silently.
 */
static void test_credit_wait_cancel_relinks_the_queue(void)
{
    struct bench b;
    CHECK(bench_init(&b), "bench setup failed");
    form(&b, 10, 0, 1);
    emit_reset();

    struct sender s1, s2, s3, s4;
    sender_init(&s1, 1);
    sender_init(&s2, 2);
    sender_init(&s3, 3);
    sender_init(&s4, 4);

    /* --- cancelling the TAIL --- */
    CHECK(scs_credit_send_or_wait(b.local, &s1.w) == SCS_CREDIT_WAIT, "s1 should block");
    CHECK(scs_credit_send_or_wait(b.local, &s2.w) == SCS_CREDIT_WAIT, "s2 should block");
    CHECK(scs_credit_send_or_wait(b.local, &s3.w) == SCS_CREDIT_WAIT, "s3 should block");
    CHECK(b.local->credit_wait_tail == &s3.w, "the tail is not the last waiter queued");

    CHECK(scs_credit_wait_cancel(b.local, &s3.w) == 0, "cancelling the tail failed");
    CHECK(b.local->credit_wait_tail == &s2.w,
          "cancelling the tail left the tail pointing at the dequeued waiter");
    CHECK(scs_credit_wait_depth(b.local) == 2, "depth %u after a tail cancel, want 2",
          scs_credit_wait_depth(b.local));

    /* The next operation to suspend must land ON THE QUEUE. */
    CHECK(scs_credit_send_or_wait(b.local, &s4.w) == SCS_CREDIT_WAIT, "s4 should block");
    CHECK(s2.w.next == &s4.w,
          "the new waiter was linked onto the cancelled node, not onto the queue -- "
          "it is unreachable from the head and can never be resumed");
    CHECK(b.local->credit_wait_tail == &s4.w, "the tail did not follow the new waiter");
    CHECK(scs_credit_wait_depth(b.local) == 3, "depth %u, want 3",
          scs_credit_wait_depth(b.local));

    /* Drain: every counted waiter must actually be reachable and resumed. */
    CHECK(scs_credit_on_recv(b.local, 3) == 0, "recv failed");
    CHECK(g_emit_n == 3, "%d resumed on a 3-credit grant, want 3 -- depth said 3", g_emit_n);
    CHECK(g_emit_order[0] == 1 && g_emit_order[1] == 2 && g_emit_order[2] == 4,
          "resume order %d,%d,%d -- want 1,2,4 (s3 cancelled)", g_emit_order[0],
          g_emit_order[1], g_emit_order[2]);
    CHECK(s4.emits == 1 && s4.w.resumed == 1,
          "the waiter queued after a tail cancel was never resumed");
    CHECK(scs_credit_wait_depth(b.local) == 0,
          "depth %u after draining everything -- a waiter is stranded in the count",
          scs_credit_wait_depth(b.local));
    CHECK(sender_silent(&s3), "a cancelled operation was resumed anyway");
    CHECK(b.local->credit_wait_tail == NULL, "the drained queue left a dangling tail");

    /* --- cancelling the HEAD, then the SOLE remaining waiter --- */
    struct sender h1, h2, h3;
    sender_init(&h1, 11);
    sender_init(&h2, 12);
    sender_init(&h3, 13);
    CHECK(b.local->send_credit == 0, "the grant was not fully consumed: %u",
          b.local->send_credit);
    CHECK(scs_credit_send_or_wait(b.local, &h1.w) == SCS_CREDIT_WAIT, "h1 should block");
    CHECK(scs_credit_send_or_wait(b.local, &h2.w) == SCS_CREDIT_WAIT, "h2 should block");
    CHECK(scs_credit_send_or_wait(b.local, &h3.w) == SCS_CREDIT_WAIT, "h3 should block");

    CHECK(scs_credit_wait_cancel(b.local, &h1.w) == 0, "cancelling the head failed");
    CHECK(b.local->credit_wait_head == &h2.w, "cancelling the head did not advance the head");
    CHECK(b.local->credit_wait_tail == &h3.w, "cancelling the head moved the tail");

    CHECK(scs_credit_wait_cancel(b.local, &h2.w) == 0, "cancel failed");
    CHECK(scs_credit_wait_cancel(b.local, &h3.w) == 0, "cancelling the sole waiter failed");
    CHECK(b.local->credit_wait_head == NULL && b.local->credit_wait_tail == NULL,
          "cancelling the last waiter left a dangling head or tail");
    CHECK(scs_credit_wait_depth(b.local) == 0, "depth %u after cancelling everything",
          scs_credit_wait_depth(b.local));

    /* The emptied queue is still usable. */
    emit_reset();
    CHECK(scs_credit_send_or_wait(b.local, &h1.w) == SCS_CREDIT_WAIT,
          "the emptied queue refused a new waiter");
    CHECK(b.local->credit_wait_head == &h1.w && b.local->credit_wait_tail == &h1.w,
          "the re-queued waiter is not both head and tail");
    CHECK(scs_credit_on_recv(b.local, 1) == 0, "recv failed");
    CHECK(g_emit_n == 1 && g_emit_order[0] == 11,
          "the waiter queued after a full cancel was not resumed (%d emits)", g_emit_n);
}

/*
 * p. 2-45: "WHENEVER the Send Credit count in a CDT is increased, the CDT's
 * queue of waiting CDRPs is examined."
 *
 * TWO calls in this module increase it. test_credit_wait_released_fifo drives
 * the receive path (scs_credit_on_recv); this drives THE OTHER ONE -- the
 * peer's grant, scs_credit_grant_from_peer, which is where the very first Send
 * Credits on a connection come from. "Whenever" means both, and a release that
 * only happens on one of them leaves an operation suspended forever on a
 * connection whose peer has already extended it credit.
 *
 * The scenario is the ordinary formation race (p. 2-43): the local SYSAP has
 * extended ITS buffers and has traffic to send, but the peer's ACCEPT_REQ --
 * which is what tells the local node how many Send Credits it has -- has not
 * arrived yet, so the sends are starved and suspend. Then it arrives.
 */
static void test_credit_wait_released_by_peer_grant(void)
{
    struct bench b;
    CHECK(bench_init(&b), "bench setup failed");
    emit_reset();

    /* Local half of formation only: no grant from the peer yet. */
    CHECK(scs_credit_extend(b.local, 10, 1) == 0, "extend failed");
    CHECK(b.local->send_credit == 0, "Send Credit %u before the peer's grant, want 0",
          b.local->send_credit);

    /* Three buffers already released back to SCS, so the first resumed send has
     * something to piggyback (p. 2-44). */
    for (int i = 0; i < 3; i++) {
        CHECK(scs_credit_release_buffer(b.local) == 0, "release %d failed", i);
    }

    struct sender s1, s2, s3;
    sender_init(&s1, 1);
    sender_init(&s2, 2);
    sender_init(&s3, 3);
    CHECK(scs_credit_send_or_wait(b.local, &s1.w) == SCS_CREDIT_WAIT, "s1 should block");
    CHECK(scs_credit_send_or_wait(b.local, &s2.w) == SCS_CREDIT_WAIT, "s2 should block");
    CHECK(scs_credit_send_or_wait(b.local, &s3.w) == SCS_CREDIT_WAIT, "s3 should block");
    CHECK(scs_credit_wait_depth(b.local) == 3, "depth %u, want 3",
          scs_credit_wait_depth(b.local));
    CHECK(g_emit_n == 0, "%d frames emitted before the peer granted anything", g_emit_n);

    /* THE PEER'S ACCEPT_REQ: it extended 2 Send Credits to us. Nothing else is
     * called -- no receive, no hand-rolled drain. */
    CHECK(scs_credit_grant_from_peer(b.local, 2) == 0, "grant failed");

    CHECK(g_emit_n == 2,
          "%d operations resumed by the peer's grant, want 2 -- p. 2-45 says EVERY "
          "rise in Send Credit examines the queue, not only the receive path",
          g_emit_n);
    CHECK(g_emit_order[0] == 1 && g_emit_order[1] == 2,
          "resume order %d,%d -- p. 2-46 requires FIFO 1,2", g_emit_order[0],
          g_emit_order[1]);
    CHECK(scs_credit_wait_depth(b.local) == 1, "depth %u after the grant, want 1",
          scs_credit_wait_depth(b.local));
    CHECK(b.local->send_credit == 0, "Send Credit %u after 2 resumes, want 0",
          b.local->send_credit);
    CHECK(sender_silent(&s3), "s3 was resumed with no credit for it");

    uint16_t v = 0xFFFF;
    CHECK(sender_frame_credit(&s1, &v) == 0 && v == 3,
          "the first resumed send carried credit %u, want 3", v);
    CHECK(sender_frame_credit(&s2, &v) == 0 && v == 0,
          "the second resumed send carried credit %u, want 0", v);
    CHECK(b.local->pending_receive_credit == 0, "Pending Receive %u, want 0",
          b.local->pending_receive_credit);
}

/*
 * p. 2-44 SPECIAL CREDIT MESSAGE, at the exact documented threshold.
 *
 * "Each time the local node receives a message on a connection, it checks to
 * see if the local Receive Credit count for the connection is 'dangerously low'.
 * If it is, AND if the local Pending Receive Credit count is greater than 0,
 * local SCS immediately sends remote SCS a special credit message containing
 * the local Pending Receive Credit count."
 * "The VMS implementation of SCS considers the local Receive Credit count to be
 * dangerously low if it is LESS THAN the sum of the local SYSGEN parameter
 * SCSFLOWCUSH and the remote value for Minimum Send Credits."
 *
 * With SCS_CREDIT_SCSFLOWCUSH = 2 and the remote's Minimum Send Credits = 3 the
 * threshold is 5. The scenario walks Receive Credit down THROUGH 5 with Pending
 * Receive Credit already > 0, so the boundary is observable: a mutant that used
 * <= instead of < fires one message early and this test catches it.
 */
static void test_special_credit_fires_at_exact_threshold(void)
{
    struct bench b;
    CHECK(bench_init(&b), "bench setup failed");
    form(&b, 10, 10, 3); /* remote Minimum Send Credits = 3 */
    emit_reset();

    const unsigned threshold = (unsigned)SCS_CREDIT_SCSFLOWCUSH + 3u;
    CHECK(threshold == 5, "threshold %u, want 5 (SCSFLOWCUSH 2 + remote minimum 3)",
          threshold);

    struct special_rec rec;
    memset(&rec, 0, sizeof(rec));
    CHECK(scs_credit_set_special_emitter(b.local, special_recorder, &rec) == 0,
          "installing the special credit emitter failed");

    int boundary_witnessed = 0;

    /*
     * ONE-WAY traffic (the case p. 2-44 introduces the mechanism for): the
     * remote keeps sending, the local SYSAP keeps releasing the buffers, and the
     * local node sends nothing back -- so nothing piggybacks and Pending Receive
     * Credit only grows.
     */
    for (int i = 1; i <= 11; i++) {
        unsigned rc_before = b.local->receive_credit;
        unsigned pend_before = b.local->pending_receive_credit;
        int fires_before = rec.fires;

        CHECK(scs_credit_on_recv(b.local, 0) == 0, "recv %d failed", i);

        /* The receive drops Receive Credit by one before the check (p. 2-44). */
        unsigned rc_at_check = rc_before > 0 ? rc_before - 1 : 0;

        if (rc_at_check == threshold) {
            /* THE BOUNDARY. Pending Receive Credit is > 0 here, so the ONLY
             * reason not to fire is that the count is not LESS THAN the sum. */
            CHECK(pend_before > 0,
                  "recv %d: boundary case is vacuous -- Pending Receive is 0", i);
            CHECK(rec.fires == fires_before,
                  "recv %d: a special credit message fired at Receive Credit == %u; "
                  "p. 2-44 says LESS THAN the sum, so the boundary is NOT low", i,
                  threshold);
            boundary_witnessed++;
        }
        if (rc_at_check < threshold && pend_before > 0) {
            CHECK(rec.fires == fires_before + 1,
                  "recv %d: Receive Credit %u < %u with Pending Receive %u and no "
                  "special credit message", i, rc_at_check, threshold, pend_before);
        }

        CHECK(scs_credit_release_buffer(b.local) == 0, "release %d failed", i);
    }

    CHECK(boundary_witnessed == 2,
          "the run passed through Receive Credit == %u exactly %d times, want 2 -- "
          "the boundary assertion above is otherwise unobservable",
          threshold, boundary_witnessed);
    CHECK(rec.fires == 2, "%d special credit messages, want 2", rec.fires);
    CHECK(rec.credit[0] == 5 && rec.credit[1] == 5,
          "special credit messages carried %u and %u, want 5 and 5", rec.credit[0],
          rec.credit[1]);
    /* The check saw Receive Credit 4 (one below the threshold); by the time the
     * message is handed to the emitter the 5 Pending Receive Credits it carries
     * have already been mirrored back into Receive Credit (p. 2-44), so 4+5=9. */
    CHECK(rec.receive_credit_at_fire[0] == 9,
          "first special fired at Receive Credit %u, want 9 (4 below the threshold, "
          "+5 mirrored by taking the Pending Receive Credit)",
          rec.receive_credit_at_fire[0]);

    /*
     * THE SECOND CONJUNCT, isolated. Keep receiving but stop releasing buffers,
     * so Pending Receive Credit stays 0 while Receive Credit falls well below
     * the threshold. p. 2-44 requires BOTH, so nothing may fire.
     */
    /* Clear the outstanding Pending Receive Credit the honest way -- the local
     * SYSAP sends one message, which piggybacks it (p. 2-44). Traffic is now
     * two-way for one message and then one-way again. */
    CHECK(scs_credit_on_send(b.local) == 1, "the piggyback should have carried 1");
    CHECK(b.local->pending_receive_credit == 0, "Pending Receive %u after a send, want 0",
          b.local->pending_receive_credit);

    int fires_before = rec.fires;
    for (int i = 0; i < 8; i++) {
        CHECK(scs_credit_on_recv(b.local, 0) == 0, "second-phase recv %d failed", i);
    }
    CHECK(b.local->pending_receive_credit == 0, "Pending Receive %u, want 0",
          b.local->pending_receive_credit);
    CHECK(scs_credit_is_dangerously_low(b.local) == 1,
          "Receive Credit %u should be dangerously low (< %u)", b.local->receive_credit,
          threshold);
    CHECK(rec.fires == fires_before,
          "%d special credit messages fired with Pending Receive Credit 0 -- p. 2-44 "
          "requires BOTH conditions", rec.fires - fires_before);

    /*
     * THE TAKER'S OWN CONTRACT for that same case, asserted directly rather
     * than through the emitter. The check above can only observe "no message
     * was emitted", and the caller inside scs_credit_on_recv() emits only when
     * the taker returns > 0 -- so it cannot tell "there is no message to take"
     * (-1) from "here is a message carrying 0". p. 2-44 makes those different
     * answers: a special credit message exists to CARRY the Pending Receive
     * Credit count, so with that count at 0 there is no message, not an empty
     * one. Nothing may move either.
     */
    unsigned rc_probe   = b.local->receive_credit;
    unsigned pend_probe = b.local->pending_receive_credit;
    CHECK(pend_probe == 0, "the probe is vacuous -- Pending Receive Credit is %u",
          pend_probe);
    CHECK(scs_credit_is_dangerously_low(b.local) == 1,
          "the probe is vacuous -- Receive Credit %u is not dangerously low",
          b.local->receive_credit);
    CHECK(scs_credit_take_special(b.local) == -1,
          "take_special handed back a message while Pending Receive Credit was 0 and "
          "Receive Credit %u was dangerously low -- p. 2-44 requires BOTH",
          b.local->receive_credit);
    CHECK(b.local->receive_credit == rc_probe &&
              b.local->pending_receive_credit == pend_probe,
          "the refused take moved a count: Receive %u (was %u), Pending %u (was %u)",
          b.local->receive_credit, rc_probe, b.local->pending_receive_credit, pend_probe);

    /* One release, one more receive: now both hold and it fires again. */
    CHECK(scs_credit_release_buffer(b.local) == 0, "release failed");
    CHECK(scs_credit_on_recv(b.local, 0) == 0, "recv failed");
    CHECK(rec.fires == fires_before + 1, "the emitter did not resume firing");
    CHECK(rec.credit[fires_before] == 1, "special carried %u, want 1",
          rec.credit[fires_before]);

    /*
     * Minimum Send Credits is an argument of CONNECT/ACCEPT and lives in the CDT
     * (p. 2-44): moving it moves the threshold and nothing else.
     */
    unsigned rc = b.local->receive_credit;
    unsigned pend = b.local->pending_receive_credit;
    int fires = rec.fires;
    CHECK(scs_credit_set_remote_min_send_credits(b.local, 0) == 0, "setter failed");
    CHECK(b.local->remote_min_send_credits == 0, "remote minimum %u, want 0",
          b.local->remote_min_send_credits);
    CHECK(b.local->receive_credit == rc && b.local->pending_receive_credit == pend,
          "changing Minimum Send Credits moved a credit count");
    CHECK(rec.fires == fires, "changing Minimum Send Credits emitted a message");
    CHECK(scs_credit_is_dangerously_low(b.local) ==
              (b.local->receive_credit < (unsigned)SCS_CREDIT_SCSFLOWCUSH ? 1 : 0),
          "the threshold did not follow Minimum Send Credits");

    scs_credit_set_special_emitter(b.local, NULL, NULL);
}

/*
 * THE ORDER OF THE TWO THINGS AN ARRIVING MESSAGE DOES (p. 2-44 / p. 2-45).
 *
 * An inbound message carrying credit does BOTH: it raises Send Credit, which
 * examines the Credit Wait queue (p. 2-45), and it triggers the dangerously-low
 * check that may send a special credit message (p. 2-44). scs_credit_on_recv()
 * runs the release FIRST, and a paragraph of its reasoning claims that ordering
 * is what makes it correct: a resumed send piggybacks the whole outstanding
 * Pending Receive Credit (p. 2-44), leaving nothing for a special credit
 * message to carry, and the special credit message exists precisely for the
 * case where nothing is going the other way ("what if it is one-way, at least
 * for awhile", p. 2-44).
 *
 * That claim is testable, and this is the test. The two phases below run the
 * SAME arrival against the SAME account state and differ only in whether an
 * operation is suspended:
 *   - CONTROL, no waiter: the message is owed and IS emitted, carrying 2. This
 *     is what makes phase 2 non-vacuous -- the conditions genuinely hold.
 *   - WITH a waiter: the resumed send carries the 2 instead, and no special
 *     credit message goes out. An implementation that checked BEFORE releasing
 *     would send both, granting the same 2 buffers twice.
 */
static void test_special_credit_yields_to_a_resumed_waiter(void)
{
    const unsigned threshold = (unsigned)SCS_CREDIT_SCSFLOWCUSH + 3u;
    CHECK(threshold == 5, "threshold %u, want 5", threshold);

    /* ---- CONTROL: the same arrival with nothing suspended ---- */
    struct bench c;
    CHECK(bench_init(&c), "bench setup failed");
    form(&c, 10, 0, 3); /* remote Minimum Send Credits 3; we are starved */
    emit_reset();

    /* Seven arrivals with no buffers released: Receive Credit falls below the
     * threshold while Pending Receive Credit is still 0. No emitter installed
     * yet, so nothing can fire during the wind-down. */
    for (int i = 0; i < 7; i++) {
        CHECK(scs_credit_on_recv(c.local, 0) == 0, "control recv %d failed", i);
    }
    CHECK(c.local->receive_credit == 3, "control Receive Credit %u, want 3",
          c.local->receive_credit);
    /* Now the SYSAP releases two buffers: from here BOTH p. 2-44 conditions hold. */
    CHECK(scs_credit_release_buffer(c.local) == 0, "control release failed");
    CHECK(scs_credit_release_buffer(c.local) == 0, "control release failed");
    CHECK(c.local->pending_receive_credit == 2, "control Pending Receive %u, want 2",
          c.local->pending_receive_credit);
    CHECK(scs_credit_is_dangerously_low(c.local) == 1, "control is not dangerously low");

    struct special_rec crec;
    memset(&crec, 0, sizeof(crec));
    CHECK(scs_credit_set_special_emitter(c.local, special_recorder, &crec) == 0,
          "installing the emitter failed");
    CHECK(scs_credit_wait_depth(c.local) == 0, "the control must have nothing suspended");

    CHECK(scs_credit_on_recv(c.local, 1) == 0, "control recv failed");
    CHECK(crec.fires == 1,
          "%d special credit messages on the control arrival, want 1 -- without this "
          "the suppression below is vacuous", crec.fires);
    CHECK(crec.credit[0] == 2, "the control's special credit message carried %u, want 2",
          crec.credit[0]);

    /* ---- THE SAME ARRIVAL, with one operation suspended ---- */
    struct bench b;
    CHECK(bench_init(&b), "bench setup failed");
    form(&b, 10, 0, 3);
    emit_reset();

    for (int i = 0; i < 7; i++) {
        CHECK(scs_credit_on_recv(b.local, 0) == 0, "recv %d failed", i);
    }
    CHECK(scs_credit_release_buffer(b.local) == 0, "release failed");
    CHECK(scs_credit_release_buffer(b.local) == 0, "release failed");

    struct sender s1;
    sender_init(&s1, 1);
    CHECK(scs_credit_send_or_wait(b.local, &s1.w) == SCS_CREDIT_WAIT, "s1 should block");

    /* Identical to the control at this point, except for the suspended send. */
    CHECK(b.local->receive_credit == 3, "Receive Credit %u, want 3 (as the control)",
          b.local->receive_credit);
    CHECK(b.local->pending_receive_credit == 2, "Pending Receive %u, want 2",
          b.local->pending_receive_credit);
    CHECK(scs_credit_is_dangerously_low(b.local) == 1, "not dangerously low");
    CHECK(scs_credit_wait_depth(b.local) == 1, "depth %u, want 1",
          scs_credit_wait_depth(b.local));

    struct special_rec rec;
    memset(&rec, 0, sizeof(rec));
    CHECK(scs_credit_set_special_emitter(b.local, special_recorder, &rec) == 0,
          "installing the emitter failed");

    CHECK(scs_credit_on_recv(b.local, 1) == 0, "recv failed");

    /* The resumed send took the whole outstanding grant with it (p. 2-44) ... */
    CHECK(s1.emits == 1, "the suspended send was not resumed (%d emits)", s1.emits);
    uint16_t v = 0xFFFF;
    CHECK(sender_frame_credit(&s1, &v) == 0 && v == 2,
          "the resumed send carried credit %u, want 2 -- a special credit message "
          "took the grant before the release", v);
    CHECK(b.local->pending_receive_credit == 0, "Pending Receive %u, want 0",
          b.local->pending_receive_credit);

    /* ... so there was nothing left for a special credit message to carry. */
    CHECK(rec.fires == 0,
          "%d special credit messages were emitted although a resumed send had "
          "already piggybacked the whole Pending Receive Credit -- the dangerously-low "
          "check must run AFTER the Credit Wait release, or the same buffers are "
          "granted to the peer twice (p. 2-44)",
          rec.fires);

    scs_credit_set_special_emitter(b.local, NULL, NULL);
    scs_credit_set_special_emitter(c.local, NULL, NULL);
}

/*
 * OVMX_NO_CREDIT_WAIT=1 -- this item's kill switch, distinct from vms-76e's
 * OVMX_NO_CREDIT_ACCOUNTING. With it set the module must revert EXACTLY to the
 * vms-76e behaviour: a starved send is refused (-1), nothing is queued, nothing
 * is resumed, and no special credit message is emitted.
 *
 * BRACKETED, per guardrail 23: the same sequence is run with the switch clear
 * BOTH before and after, so a green run cannot come from the scenario never
 * having produced the behaviour in the first place.
 */
static void test_credit_wait_kill_switch(void)
{
    struct sender s;
    struct bench  b;

    /* --- bracket 1: switch CLEAR. The behaviour must be present. --- */
    scs_credit_reset_switch_cache();
    CHECK(scs_credit_wait_enabled() == 1, "Credit Wait should start enabled");
    CHECK(bench_init(&b), "bench setup failed");
    form(&b, 10, 0, 3);
    emit_reset();
    sender_init(&s, 1);
    CHECK(scs_credit_send_or_wait(b.local, &s.w) == SCS_CREDIT_WAIT,
          "control: a starved send must block");
    CHECK(scs_credit_wait_depth(b.local) == 1, "control: depth %u, want 1",
          scs_credit_wait_depth(b.local));
    CHECK(scs_credit_on_recv(b.local, 1) == 0, "control: recv failed");
    CHECK(s.emits == 1, "control: the waiter was not resumed by the grant");

    /* --- switch SET --- */
    if (setenv("OVMX_NO_CREDIT_WAIT", "1", 1) != 0) {
        CHECK(0, "setenv failed");
        return;
    }
    scs_credit_reset_switch_cache();
    CHECK(scs_credit_wait_enabled() == 0, "the kill switch did not disable Credit Wait");
    CHECK(scs_credit_enabled() == 1,
          "OVMX_NO_CREDIT_WAIT must not disable the vms-76e account as well");

    CHECK(bench_init(&b), "bench setup failed");
    form(&b, 10, 0, 3);
    emit_reset();
    sender_init(&s, 1);

    /* No queue. The vms-76e refusal, byte for byte. */
    CHECK(scs_credit_send_or_wait(b.local, &s.w) == -1,
          "under the switch a starved send must be refused, not suspended");
    CHECK(scs_credit_wait_depth(b.local) == 0, "under the switch the queue must stay empty");
    CHECK(s.w.queued == 0, "under the switch the waiter must not be linked");
    CHECK(sender_silent(&s), "under the switch a refused send emitted a frame");
    CHECK(scs_credit_wait_release(b.local) == 0, "under the switch nothing may be resumed");

    /* Credit still arrives and is still accounted -- only the WAIT is gone. */
    CHECK(scs_credit_on_recv(b.local, 2) == 0, "recv failed");
    CHECK(b.local->send_credit == 2, "Send Credit %u under the switch, want 2",
          b.local->send_credit);
    CHECK(s.emits == 0, "under the switch a grant resumed something");

    /* And no special credit message is emitted, even though the conditions
     * genuinely hold -- proved by taking the message by hand afterwards. */
    struct special_rec gated;
    memset(&gated, 0, sizeof(gated));
    CHECK(scs_credit_set_special_emitter(b.local, special_recorder, &gated) == 0,
          "installing the emitter failed");
    for (int i = 0; i < 7; i++) {
        CHECK(scs_credit_on_recv(b.local, 0) == 0, "recv %d failed", i);
        CHECK(scs_credit_release_buffer(b.local) == 0, "release %d failed", i);
    }
    CHECK(gated.fires == 0, "%d special credit messages emitted under the switch",
          gated.fires);
    CHECK(scs_credit_is_dangerously_low(b.local) == 1,
          "the scenario did not actually reach a dangerously low Receive Credit (%u) -- "
          "the suppression above would be vacuous",
          b.local->receive_credit);
    CHECK(b.local->pending_receive_credit > 0,
          "the scenario left Pending Receive Credit at 0 -- suppression vacuous");
    CHECK(scs_credit_take_special(b.local) > 0,
          "a special credit message WAS owed; only its emission is gated");

    /* --- bracket 2: switch CLEAR again. The behaviour must come back. --- */
    unsetenv("OVMX_NO_CREDIT_WAIT");
    scs_credit_reset_switch_cache();
    CHECK(scs_credit_wait_enabled() == 1, "clearing the switch did not re-enable Credit Wait");

    CHECK(bench_init(&b), "bench setup failed");
    form(&b, 10, 0, 3);
    emit_reset();
    sender_init(&s, 1);
    CHECK(scs_credit_send_or_wait(b.local, &s.w) == SCS_CREDIT_WAIT,
          "bracket 2: a starved send must block again");
    CHECK(scs_credit_on_recv(b.local, 1) == 0, "bracket 2: recv failed");
    CHECK(s.emits == 1, "bracket 2: the waiter was not resumed after clearing the switch");
}

/* --- kill switch ---------------------------------------------------------- */

/*
 * OVMX_NO_CREDIT_ACCOUNTING=1 suppresses the whole account. Re-runs the p. 2-44
 * worked example with the switch set and asserts every count stays at its
 * initial value and the wire field is left untouched.
 */
static void test_kill_switch(void)
{
    if (setenv("OVMX_NO_CREDIT_ACCOUNTING", "1", 1) != 0) {
        CHECK(0, "setenv failed");
        return;
    }
    scs_credit_reset_switch_cache();
    CHECK(scs_credit_enabled() == 0, "kill switch did not disable the accounting");

    struct bench b;
    CHECK(bench_init(&b), "bench setup failed");
    form(&b, 10, 10, 1);

    /* Nothing was extended, nothing entered the MFREEQ. */
    CHECK(b.local->extended_credits == 0, "extended_credits %u under the switch, want 0",
          b.local->extended_credits);
    CHECK(b.local->receive_credit == 0, "receive_credit %u under the switch, want 0",
          b.local->receive_credit);
    CHECK(b.remote->send_credit == 0, "send_credit %u under the switch, want 0",
          b.remote->send_credit);
    CHECK(b.pdt_l.mfreeq_count == 0, "MFREEQ %u under the switch, want 0",
          b.pdt_l.mfreeq_count);

    /* Sends never block and never piggyback; receives never add; releases never
     * accrue. This is the entire p. 2-44 example, suppressed. */
    for (int i = 0; i < 3; i++) {
        CHECK(scs_credit_can_send(b.remote) == 1, "can_send must not block under the switch");
        CHECK(scs_credit_on_send(b.remote) == 0, "on_send must carry 0 under the switch");
        CHECK(scs_credit_on_recv(b.local, 3) == 0, "on_recv failed");
        CHECK(scs_credit_release_buffer(b.local) == 0, "release failed");
    }
    CHECK(b.remote->send_credit == 0, "send_credit moved under the switch: %u",
          b.remote->send_credit);
    CHECK(b.local->pending_receive_credit == 0, "pending_receive moved under the switch: %u",
          b.local->pending_receive_credit);
    CHECK(b.local->receive_credit == 0, "receive_credit moved under the switch: %u",
          b.local->receive_credit);
    CHECK(scs_credit_is_dangerously_low(b.local) == 0, "no low report under the switch");
    CHECK(scs_credit_take_special(b.local) == -1, "no special message under the switch");

    /* The wire half: stamping writes NOTHING, so a frame keeps its bytes. */
    uint8_t out[190];
    memcpy(out, frame190_vc, sizeof(out));
    CHECK(scs_credit_stamp_header(out, sizeof(out), 7) == 0, "stamp returns 0 under the switch");
    CHECK(memcmp(out, frame190_vc, sizeof(out)) == 0,
          "the kill switch must leave the frame byte-identical");
    /* Reading is unaffected -- reading a peer's credit is never wrong. */
    uint16_t v = 0;
    CHECK(scs_credit_read_header(frame190_vc, sizeof(frame190_vc), &v) == 0 && v == 3,
          "read must still work under the switch");

    /* Restore, and prove the switch is what did it: the same sequence accounts
     * normally once it is cleared. */
    unsetenv("OVMX_NO_CREDIT_ACCOUNTING");
    scs_credit_reset_switch_cache();
    CHECK(scs_credit_enabled() == 1, "clearing the switch did not re-enable the accounting");

    struct bench b2;
    CHECK(bench_init(&b2), "bench setup failed");
    form(&b2, 10, 10, 1);
    CHECK(b2.local->receive_credit == 10, "control: receive_credit %u, want 10",
          b2.local->receive_credit);
    CHECK(b2.pdt_l.mfreeq_count == 10, "control: MFREEQ %u, want 10", b2.pdt_l.mfreeq_count);
}

int main(void)
{
    test_formation_extends_credits();
    test_worked_example_p2_43();
    test_worked_example_variation_p2_44();
    test_credit_wait_p2_45();
    test_special_credit_message_p2_44();
    test_wire_credit_field();

    /* vms-1d2 */
    test_credit_wait_blocks_and_does_not_emit();
    test_credit_wait_released_fifo();
    test_credit_wait_order_is_preserved();
    test_credit_wait_late_sender_does_not_overtake();
    test_credit_wait_cancel_relinks_the_queue();
    test_credit_wait_released_by_peer_grant();
    test_special_credit_fires_at_exact_threshold();
    test_special_credit_yields_to_a_resumed_waiter();
    test_credit_wait_kill_switch(); /* mutates the environment; restores it */

    test_kill_switch(); /* must run last: it mutates the environment */

    printf("%s: %d checks, %d failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
