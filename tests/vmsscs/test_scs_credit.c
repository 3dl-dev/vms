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
     * back -- so nothing is piggybacked and Receive Credit falls. */
    unsigned sent = 0;
    while (b.local->receive_credit >= threshold) {
        int c = scs_credit_on_send(b.remote);
        CHECK(c >= 0, "remote send %u refused", sent);
        scs_credit_on_recv(b.local, (unsigned)c);
        scs_credit_release_buffer(b.local);
        sent++;
    }
    CHECK(b.local->receive_credit == threshold - 1, "Receive Credit %u, want %u",
          b.local->receive_credit, threshold - 1);
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
    test_kill_switch(); /* must run last: it mutates the environment */

    printf("%s: %d checks, %d failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
