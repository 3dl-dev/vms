/*
 * test_scs_member.c - targeted unit tests for the VMS$VAXcluster
 * connection-manager add-member transaction builders/parsers (vms-224).
 *
 * ORACLE. Every expected byte array below is the byte-exact SCA content of a
 * real frame from formation-ci1-joinwindow.pcap (the golden VAX2-joins-VAX1
 * handshake), the ground-truth reference cluster. The builders are validated by
 * reproducing those captured joiner frames byte-for-byte, and the 0x81
 * response is validated by asserting it echoes the member's real captured
 * (txn, checksum) token -- proving OVMX never derives or fakes the checksum
 * (spec sec 4j: the joiner echoes; it does not compute).
 */
#include "scs_member.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* Golden SCA content (190 bytes each) -- indices are SCA-content offsets, so
 * the SYSAP body starts at [58]. */

/* op 0x14 node model advertisement -- SCA#48 (VAX2->VAX1, sendmsg=1). */
static const uint8_t golden_op14[190] = {
    0xbc,0x00,0xaa,0x00,0x04,0x00,0x01,0x04,0x01,0x00,0xaa,0x00,
    0x04,0x00,0x02,0x04,0x4b,0x13,0x0a,0x00,0x0a,0x00,0x01,0x00,
    0x12,0x00,0x0a,0x00,0x00,0x00,0x0a,0x00,0x00,0x00,0x0a,0x00,
    0x00,0x00,0x01,0x00,0x00,0x02,0x92,0x00,0x04,0x00,0x0a,0x00,
    0x00,0x00,0x09,0x00,0xc5,0x62,0x08,0x00,0x58,0x33,0x01,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x14,0x00,0x00,0x00,0x00,
    0x00,0x00,0x15,0x56,0x41,0x58,0x73,0x65,0x72,0x76,0x65,0x72,
    0x20,0x33,0x39,0x30,0x30,0x20,0x53,0x65,0x72,0x69,0x65,0x73,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

/* op 0x01 cluster parameters -- SCA#49 (VAX2->VAX1, sendmsg=2, VOTES 0). */
static const uint8_t golden_op01[190] = {
    0xbc,0x00,0xaa,0x00,0x04,0x00,0x01,0x04,0x01,0x00,0xaa,0x00,
    0x04,0x00,0x02,0x04,0x4b,0x13,0x0a,0x00,0x0b,0x00,0x01,0x00,
    0x12,0x00,0x0a,0x00,0x00,0x00,0x0b,0x00,0x00,0x00,0x0a,0x00,
    0x00,0x00,0x01,0x00,0x00,0x02,0x92,0x00,0x04,0x00,0x0a,0x00,
    0x00,0x00,0x09,0x00,0xc5,0x62,0x08,0x00,0x58,0x33,0x02,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x00,0x00,0x50,
    0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x80,0x4a,0x3f,0x0e,0x57,0x9f,0x00,0x10,0x00,
    0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x2a,0x00,0x0e,0x00,
    0x00,0x00,0x56,0x37,0x2e,0x33,0x20,0x20,0x20,0x20,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

/* op 0x02 config -- SCA#60 (VAX2->VAX1, sendmsg=3, ackmsg=2). */
static const uint8_t golden_op02[190] = {
    0xbc,0x00,0xaa,0x00,0x04,0x00,0x01,0x04,0x01,0x00,0xaa,0x00,
    0x04,0x00,0x02,0x04,0x4b,0x13,0x0f,0x00,0x10,0x00,0x01,0x00,
    0x12,0x00,0x0f,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x0f,0x00,
    0x00,0x00,0x01,0x00,0x00,0x02,0x92,0x00,0x04,0x00,0x0a,0x00,
    0x02,0x00,0x09,0x00,0xc5,0x62,0x08,0x00,0x58,0x33,0x03,0x00,
    0x02,0x00,0x00,0x00,0x00,0x00,0x01,0x02,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

/* op 0x03 commit REQUEST from member (VAX1) -- SCA#62 (txn=0x0001,csum=0x5f23). */
static const uint8_t golden_op03_req[190] = {
    0xbc,0x00,0xaa,0x00,0x04,0x00,0x02,0x04,0x01,0x00,0xaa,0x00,
    0x04,0x00,0x01,0x04,0x4b,0x13,0x10,0x00,0x11,0x00,0x01,0x00,
    0x12,0x00,0x10,0x00,0x00,0x00,0x11,0x00,0x00,0x00,0x10,0x00,
    0x00,0x00,0x01,0x00,0x00,0x02,0x92,0x00,0x04,0x00,0x0a,0x00,
    0x01,0x00,0x08,0x00,0x58,0x33,0x09,0x00,0xc5,0x62,0x04,0x00,
    0x03,0x00,0x01,0x00,0x23,0x5f,0x01,0x03,0x00,0x00,0x03,0x00,
    0x00,0x00,0x20,0x02,0x00,0x00,0x80,0x40,0x13,0x56,0x96,0x00,
    0xbc,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

/* op 0x05 lock-rebuild REQUEST from member (VAX1) -- SCA#64 (txn=0x0004,csum=0x554b). */
static const uint8_t golden_op05_req[190] = {
    0xbc,0x00,0xaa,0x00,0x04,0x00,0x02,0x04,0x01,0x00,0xaa,0x00,
    0x04,0x00,0x01,0x04,0x4b,0x13,0x11,0x00,0x12,0x00,0x01,0x00,
    0x12,0x00,0x11,0x00,0x00,0x00,0x12,0x00,0x00,0x00,0x11,0x00,
    0x00,0x00,0x01,0x00,0x00,0x02,0x92,0x00,0x04,0x00,0x0a,0x00,
    0x01,0x00,0x08,0x00,0x58,0x33,0x09,0x00,0xc5,0x62,0x05,0x00,
    0x04,0x00,0x04,0x00,0x4b,0x55,0x01,0x05,0x00,0x00,0x03,0x00,
    0x00,0x00,0x20,0x02,0x00,0x00,0x01,0x04,0x00,0x00,0x00,0x00,
    0x00,0x00,0x66,0x15,0x66,0x7a,0x93,0x00,0xbc,0x00,0x01,0x00,
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

/* VAX2 (joiner) logical + VAX1 (member) logical for byte-exact reproduction. */
static const uint8_t vax2_logical[6] = {0xaa,0x00,0x04,0x00,0x02,0x04};
static const uint8_t vax1_logical[6] = {0xaa,0x00,0x04,0x00,0x01,0x04};

#define VAX1_VC_CONID 0x62c50009u  /* member's VMS$VAXcluster Con.ID */
#define VAX2_VC_CONID 0x33580008u  /* joiner's (OVMX-role) Con.ID */

/* Fill params to reproduce a joiner frame VAX2->VAX1. */
static void joiner_params(struct scs_member_params *mp, uint16_t recv_ack,
                          uint16_t send_seq, uint16_t sysap_send, uint16_t sysap_ack)
{
    memset(mp, 0, sizeof(*mp));
    memcpy(mp->dst_mac, vax1_logical, 6);     /* Ethernet header unused by SCA compare */
    memcpy(mp->src_mac, vax2_logical, 6);     /* Ethernet src (VAX2 is a DECnet node here) */
    memcpy(mp->src_logical, vax2_logical, 6); /* SCA src-logical [10:16] (abs 24) = VAX2 logical (vms-9f3) */
    memcpy(mp->peer_logical, vax1_logical, 6);
    mp->remote_conid = VAX1_VC_CONID;
    mp->local_conid = VAX2_VC_CONID;
    mp->recv_ack = recv_ack;
    mp->send_seq = send_seq;
    mp->incarnation = 1; /* fresh-formation value; matches the golden templates */
    mp->sysap_send_msg = sysap_send;
    mp->sysap_ack_msg = sysap_ack;
}

static void test_op14_byte_exact(void)
{
    struct scs_member_params mp;
    joiner_params(&mp, 10, 10, 1, 0);
    mp.model = "VAXserver 3900 Series"; /* reproduce the golden model string */
    uint8_t out[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_model(&mp, out) == 0, "build_model ok");
    CHECK(memcmp(out + 14, golden_op14, 190) == 0,
          "op 0x14 reproduces golden SCA#48 byte-exact");
}

static void test_op01_byte_exact_and_votes(void)
{
    struct scs_member_params mp;
    joiner_params(&mp, 10, 11, 2, 0);
    mp.votes = 0; /* VAX2 joined non-voting */
    uint8_t out[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_params(&mp, out) == 0, "build_params ok");
    CHECK(memcmp(out + 14, golden_op01, 190) == 0,
          "op 0x01 (VOTES=0) reproduces golden SCA#49 byte-exact");
    /* VOTES field is at SYSAP body[22:24] = SCA[58+22]=SCA[80] = abs frame 94. */
    CHECK(out[14 + 58 + 22] == 0x00 && out[14 + 58 + 23] == 0x00,
          "VOTES=0 at body[22:24] (abs 94)");

    /* VOTES=2 must change ONLY body[22:24] (spec sec 4j grounded diff). */
    uint8_t out2[SCS_MEMBER_FRAME_LEN];
    mp.votes = 2;
    CHECK(scs_member_build_params(&mp, out2) == 0, "build_params votes=2 ok");
    CHECK(out2[14 + 58 + 22] == 0x02 && out2[14 + 58 + 23] == 0x00,
          "VOTES=2 at body[22:24]");
    int diffs = 0, diff_off = -1;
    for (int i = 14; i < SCS_MEMBER_FRAME_LEN; i++) {
        if (out[i] != out2[i]) { diffs++; diff_off = i; }
    }
    CHECK(diffs == 1 && diff_off == 14 + 58 + 22,
          "VOTES 0->2 changes exactly one byte, at body[22] (abs 94)");
}

static void test_op02_byte_exact(void)
{
    struct scs_member_params mp;
    joiner_params(&mp, 15, 16, 3, 2);
    uint8_t out[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_config(&mp, out) == 0, "build_config ok");
    CHECK(memcmp(out + 14, golden_op02, 190) == 0,
          "op 0x02 reproduces golden SCA#60 byte-exact");
}

static void test_default_model_is_ovmx(void)
{
    struct scs_member_params mp;
    joiner_params(&mp, 10, 10, 1, 0);
    mp.model = NULL; /* OVMX default */
    uint8_t out[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_model(&mp, out) == 0, "build_model default ok");
    const uint8_t *body = out + 72; /* SYSAP body[0] = abs 72 */
    uint8_t mlen = body[16];
    CHECK(mlen > 0 && mlen <= 115, "OVMX model length prefix sane");
    /* OVMX's own model string -- honest, NOT a VAXserver (INV-0). */
    CHECK(memcmp(body + 17, "OVMX", 4) == 0,
          "default model string identifies OVMX (not a faked VAX)");
    /* Envelope + SYSAP header still correct. */
    CHECK(out[30] == SCS_MEMBER_MSGTYPE && out[31] == SCS_MEMBER_FORMAT,
          "default-model frame keeps SCS envelope 0x4b/0x13");
    CHECK(body[8] == SCS_MEMBER_CAT_CONFIG && body[9] == SCS_MEMBER_OP_MODEL,
          "default-model frame is category 0x01 op 0x14");
}

static void make_frame(uint8_t out[SCS_MEMBER_FRAME_LEN], const uint8_t sca[190])
{
    memset(out, 0, SCS_MEMBER_FRAME_LEN);
    /* dummy Ethernet header + ethertype */
    out[12] = 0x60; out[13] = 0x07;
    memcpy(out + 14, sca, 190);
}

static void test_parse_classification(void)
{
    uint8_t frame[SCS_MEMBER_FRAME_LEN];
    struct scs_member_view v;

    make_frame(frame, golden_op14);
    CHECK(scs_member_parse(frame, sizeof(frame), &v) == 0, "parse op14 ok");
    CHECK(v.category == 0x01 && v.opcode == 0x14 && !v.is_response &&
          !v.is_member_txn, "op 0x14 is config, not a member txn");

    make_frame(frame, golden_op01);
    CHECK(scs_member_parse(frame, sizeof(frame), &v) == 0, "parse op01 ok");
    CHECK(v.category == 0x01 && v.opcode == 0x01 && !v.is_member_txn,
          "op 0x01 is config, not a member txn");
    CHECK(v.sysap_send_msg == 2 && v.sysap_ack_msg == 0,
          "op 0x01 SYSAP send/ack-msg# parsed");

    make_frame(frame, golden_op03_req);
    CHECK(scs_member_parse(frame, sizeof(frame), &v) == 0, "parse op03 req ok");
    CHECK(v.category == 0x01 && v.opcode == 0x03 && !v.is_response &&
          v.is_member_txn, "op 0x03 member request IS a member txn");
    CHECK(v.txn == 0x0001 && v.checksum == 0x5f23,
          "op 0x03 (txn,checksum) parsed from real frame");

    make_frame(frame, golden_op05_req);
    CHECK(scs_member_parse(frame, sizeof(frame), &v) == 0, "parse op05 req ok");
    CHECK(v.opcode == 0x05 && v.is_member_txn &&
          v.txn == 0x0004 && v.checksum == 0x554b,
          "op 0x05 member request IS a member txn with real (txn,checksum)");
}

/* The checksum handling test the task requires: OVMX ECHOES the member's real
 * captured (txn, checksum) token in its 0x81 response -- it never derives or
 * fakes it. Asserted against the real captured frame. */
static void test_response_echoes_real_checksum(void)
{
    uint8_t req[SCS_MEMBER_FRAME_LEN];
    make_frame(req, golden_op03_req);

    struct scs_member_params mp;
    memset(&mp, 0, sizeof(mp));
    memcpy(mp.dst_mac, vax1_logical, 6);
    memcpy(mp.src_mac, vax2_logical, 6);
    memcpy(mp.src_logical, vax2_logical, 6); /* SCA src-logical (abs 24) = VAX2 logical (vms-9f3) */
    memcpy(mp.peer_logical, vax1_logical, 6);
    mp.remote_conid = VAX1_VC_CONID;
    mp.local_conid = VAX2_VC_CONID;
    mp.incarnation = 1;
    mp.recv_ack = 17;
    mp.send_seq = 17;
    mp.sysap_send_msg = 4;
    mp.sysap_ack_msg = 4; /* ack the member's send-msg#=4 */

    uint8_t out[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_response(&mp, req, sizeof(req), out) == 0,
          "build_response op03 ok");

    const uint8_t *obody = out + 72;
    const uint8_t *rbody = req + 72;

    /* GROUNDED transform (spec sec 4j). */
    CHECK(out[30] == SCS_MEMBER_MSGTYPE && out[31] == SCS_MEMBER_FORMAT,
          "response SCS envelope 0x4b/0x13");
    CHECK(obody[8] == (SCS_MEMBER_CAT_CONFIG | SCS_MEMBER_RESPONSE_BIT),
          "response category is 0x81 (response bit set)");
    CHECK(obody[9] == 0x03, "response echoes opcode 0x03");
    /* THE CHECKSUM: echoed byte-for-byte from the member's real frame, NOT
     * computed. txn body[4:6]=0x0001, checksum body[6:8]=0x5f23. */
    CHECK(obody[4] == rbody[4] && obody[5] == rbody[5],
          "response echoes real transaction number (no fabrication)");
    CHECK(obody[6] == rbody[6] && obody[7] == rbody[7],
          "response echoes the REAL captured checksum 0x5f23 (echo, not derive)");
    CHECK((uint16_t)(obody[6] | (obody[7] << 8)) == 0x5f23,
          "echoed checksum equals the captured value 0x5f23");
    /* SYSAP header + response marker. */
    CHECK((uint16_t)(obody[0] | (obody[1] << 8)) == 4, "response send-msg#=4");
    CHECK((uint16_t)(obody[2] | (obody[3] << 8)) == 4, "response ack-msg#=4");
    CHECK(obody[SCS_MEMBER_RESP_MARK_BODYOFF] == 0x01,
          "response marker body[18]=0x01 (GROUNDED)");
    /* Con.ID pair swapped: OVMX addresses the member. */
    CHECK((uint32_t)(out[64] | (out[65]<<8) | (out[66]<<16) | ((uint32_t)out[67]<<24))
              == VAX1_VC_CONID, "response remote Con.ID = member's");
    CHECK((uint32_t)(out[68] | (out[69]<<8) | (out[70]<<16) | ((uint32_t)out[71]<<24))
              == VAX2_VC_CONID, "response local Con.ID = OVMX's");

    /* op 0x05 response likewise echoes its real checksum 0x554b. */
    uint8_t req5[SCS_MEMBER_FRAME_LEN], out5[SCS_MEMBER_FRAME_LEN];
    make_frame(req5, golden_op05_req);
    mp.sysap_send_msg = 5; mp.sysap_ack_msg = 5; mp.recv_ack = 20; mp.send_seq = 18;
    CHECK(scs_member_build_response(&mp, req5, sizeof(req5), out5) == 0,
          "build_response op05 ok");
    const uint8_t *o5 = out5 + 72;
    CHECK(o5[8] == (SCS_MEMBER_CAT_CONFIG | SCS_MEMBER_RESPONSE_BIT) && o5[9] == 0x05,
          "op05 response is 0x81 opcode 0x05");
    CHECK((uint16_t)(o5[6] | (o5[7] << 8)) == 0x554b,
          "op05 response echoes the REAL captured checksum 0x554b");
}

static void test_null_guards(void)
{
    uint8_t out[SCS_MEMBER_FRAME_LEN];
    struct scs_member_params mp;
    memset(&mp, 0, sizeof(mp));
    CHECK(scs_member_build_model(NULL, out) == -1, "build_model NULL p");
    CHECK(scs_member_build_model(&mp, NULL) == -1, "build_model NULL out");
    CHECK(scs_member_build_params(NULL, out) == -1, "build_params NULL p");
    CHECK(scs_member_build_config(NULL, out) == -1, "build_config NULL p");
    struct scs_member_view v;
    CHECK(scs_member_parse(NULL, 204, &v) == -1, "parse NULL frame");
    uint8_t shortf[10] = {0};
    CHECK(scs_member_parse(shortf, sizeof(shortf), &v) == -1, "parse short frame");
    /* A too-long model string is rejected. */
    char big[200];
    memset(big, 'X', sizeof(big));
    big[199] = '\0';
    mp.model = big;
    memcpy(mp.src_mac, vax2_logical, 6);
    CHECK(scs_member_build_model(&mp, out) == -1, "build_model rejects oversized model");
}

/*
 * vms-e1a, p. 2-35: "each packet contains source and destination CONIDs. These
 * quantities come from the CDT used by SCS on the source node to describe the
 * connection between the two SYSAPs. The source CONID comes from the local
 * CONID field of that CDT; and the destination CONID comes from the remote
 * CONID field of the CDT."
 *
 * The 190-byte VMS$VAXcluster class is the steady-state message class -- it is
 * 2899 of the 3000 frames in formation-ci1-joinwindow.pcap and 18296 of 19930
 * in vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap -- and in every one of
 * those observed frames BOTH Con.IDs are non-zero. Pin that on all four things
 * this module builds, so the pair can be sourced from a CDT's local_conid /
 * remote_conid fields with no wire change (scs_cdt.h WIRE VERDICT).
 */
static uint32_t m_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void test_both_conids_p235(void)
{
    struct scs_member_params mp;
    uint8_t out[SCS_MEMBER_FRAME_LEN];

    struct {
        const char *what;
        int (*build)(const struct scs_member_params *, uint8_t *);
    } builders[3] = {
        {"op 0x14 model", scs_member_build_model},
        {"op 0x01 params", scs_member_build_params},
        {"op 0x02 config", scs_member_build_config},
    };

    for (unsigned i = 0; i < 3; i++) {
        joiner_params(&mp, 10, 10, 1, 0);
        CHECK(builders[i].build(&mp, out) == 0, "build for the p.2-35 Con.ID check");
        CHECK(m_le32(out + 64) == VAX1_VC_CONID,
              "190-byte frame destination Con.ID == remote CDT's CONID (abs 64)");
        CHECK(m_le32(out + 68) == VAX2_VC_CONID,
              "190-byte frame source Con.ID == local CDT's CONID (abs 68)");
        CHECK(m_le32(out + 64) != 0 && m_le32(out + 68) != 0,
              "190-byte frame carries BOTH Con.IDs non-zero (p. 2-35)");
        (void)builders[i].what;
    }

    /* The 0x81 response to a member transaction carries the pair too. */
    joiner_params(&mp, 20, 21, 5, 4);
    uint8_t req[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_config(&mp, req) == 0, "build a stand-in request frame");
    CHECK(scs_member_build_response(&mp, req, sizeof(req), out) == 0, "build_response ok");
    CHECK(m_le32(out + 64) == VAX1_VC_CONID && m_le32(out + 68) == VAX2_VC_CONID,
          "0x81 response carries BOTH Con.IDs from the CDT's pair (p. 2-35)");

    /* The parser reads the same two fields back, so a receiver can use the
     * destination Con.ID to find its CDT (p. 2-29) and the source Con.ID to
     * check the sender (p. 2-35). */
    struct scs_member_view mv;
    CHECK(scs_member_parse(out, sizeof(out), &mv) == 0, "parse our own 0x81 response");
    CHECK(mv.remote_conid == VAX1_VC_CONID && mv.local_conid == VAX2_VC_CONID,
          "parsed Con.ID pair round-trips");
}

/* =====================================================================
 * vms-578 INTEGRATION: test functions added by worktree-760.
 * Both branches only APPENDED here, so nothing is replaced -- these are
 * carried over verbatim and registered in main() below.
 * ===================================================================== */


/*
 * vms-760: the op-0x02 body carries ZEROS at body[10:12] and body[40:52].
 * An earlier revision replayed 0x5041 and twelve spaces there, copied from the
 * single admission specimen in vax3-2to3-established-join-20260730.pcap
 * (frame 285). A survey of every op-0x02 in the capture library retired that:
 * 9 of 12 genuine VMS specimens carry zeros in both places and are acked
 * identically, and the 3 outliers hold printable digraphs ("AP", "IS") and
 * ASCII spaces -- stale buffer contents, not fields. This test pins that we do
 * NOT reproduce another implementation's uninitialised memory.
 */
static void test_op02_residue_fields_are_zero(void)
{
    struct scs_member_params mp;
    joiner_params(&mp, 15, 16, 3, 2);
    uint8_t out[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_config(&mp, out) == 0, "build_config ok");
    CHECK(out[82] == 0x00 && out[83] == 0x00,
          "op 0x02 body[10:12] is zero, not the \"AP\" residue");
    int zeros = 1;
    for (int i = 112; i < 124; i++) {
        if (out[i] != 0x00) {
            zeros = 0;
        }
    }
    CHECK(zeros, "op 0x02 body[40:52] is zero, not twelve ASCII spaces");
}

/*
 * vms-2f3: the op-0x02 REJOIN form. A returning node's admission request is a
 * DIFFERENT SHAPE from a first-timer's, and OVMX sent the first-timer's on
 * every rejoin it ever attempted.
 *
 * Pinned to the real crash-rejoin specimen
 * (captures/vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap frame #1297, a real
 * VAX3 that was kill -9'd, class-0x03 crash-removed, and rebooted under an
 * unchanged SCSNODE/SCSSYSTEMID): body[20:22]=1, body[22:24]=1025 (the founding
 * node's SCSSYSTEMID, = SDA's CLUB `Found Node SYSID 000000000401`) and
 * body[28:36]=004af82e3605bc00 (the cluster founding time, = SDA's CLUB
 * `Founding Time 1-AUG-2026 12:03:09`, which that quadword decodes to exactly).
 *
 * The FIRST-JOIN form must be preserved: two real first joins (vax3-2to3 #285,
 * formation-ci1 #67) carry zeros in all three, so a genuinely fresh identity
 * has to keep sending zeros. Both directions are asserted here because the
 * conditional is the whole point -- always sending the rejoin form would be as
 * wrong as never sending it.
 */
static void test_op02_rejoin_form(void)
{
    struct scs_member_params mp;
    joiner_params(&mp, 15, 16, 3, 2);
    uint8_t first[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_config(&mp, first) == 0, "build_config first-join ok");

    const int B = 14 + 58; /* body[0] absolute */
    CHECK(first[B + 20] == 0 && first[B + 21] == 0 &&
          first[B + 22] == 0 && first[B + 23] == 0,
          "first-join op 0x02 keeps body[20:24] zero");
    int ft_zero = 1;
    for (int i = 0; i < 8; i++) {
        if (first[B + 28 + i] != 0) {
            ft_zero = 0;
        }
    }
    CHECK(ft_zero, "first-join op 0x02 keeps body[28:36] zero");

    /* The rejoin form, with the crash-rejoin specimen's own values. */
    static const uint8_t founding_time[8] = {
        0x00, 0x4a, 0xf8, 0x2e, 0x36, 0x05, 0xbc, 0x00
    };
    uint64_t formed = 0;
    for (int k = 7; k >= 0; k--) {
        formed = (formed << 8) | founding_time[k];
    }
    mp.rejoin = 1;
    mp.founding_sysid = 1025;
    mp.cluster_formed = formed;

    /* vms-e15: FAIL-PRE. A rejoin with generation 0 (the historical form OVMX
     * emitted, and what OVMX_REJOIN_GEN=0 forces) leaves body[36:40] zero. Every
     * real rejoin carries a NON-ZERO body[36:40] (9/2/2/3 across four specimens);
     * a full byte-diff vs the SUCCESS oracle (frame 1297) leaves [36:40] the sole
     * rejoin-discriminating body field OVMX omitted. NOTE: this is a WIRE-SHAPE
     * test -- filling [36:40] is grounded but does NOT by itself complete
     * readmission (spec sec 4(O.10), proven live; blocker deferred to vms-694). */
    mp.rejoin_generation = 0;
    uint8_t rej0[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_config(&mp, rej0) == 0, "build_config rejoin gen=0 ok");
    CHECK(rej0[B + 36] == 0 && rej0[B + 37] == 0 &&
          rej0[B + 38] == 0 && rej0[B + 39] == 0,
          "PRE: rejoin with generation 0 leaves body[36:40] zero (the historical form)");

    /* PASS-POST. A rejoin carrying a membership generation (here 2, the second
     * membership of a once-admitted identity) fills body[36:40] with that LE
     * longword, matching the shape of every real rejoin specimen. */
    mp.rejoin_generation = 2;
    uint8_t rej[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_config(&mp, rej) == 0, "build_config rejoin ok");
    CHECK(rej[B + 20] == 0x01 && rej[B + 21] == 0x00,
          "rejoin op 0x02 body[20:22] = 1 (prior cluster state)");
    CHECK(rej[B + 22] == 0x01 && rej[B + 23] == 0x04,
          "rejoin op 0x02 body[22:24] = 1025, the founding node's SCSSYSTEMID");
    CHECK(memcmp(rej + B + 28, founding_time, 8) == 0,
          "rejoin op 0x02 body[28:36] = the cluster founding time, verbatim");
    CHECK(rej[B + 36] == 0x02 && rej[B + 37] == 0 &&
          rej[B + 38] == 0 && rej[B + 39] == 0,
          "POST: rejoin op 0x02 body[36:40] = the membership generation, non-zero");

    /* [40:52] stays zero -- it varies (spaces/zeros) across successful rejoins,
     * so it carries nothing the member needs; deliberately not moved. */
    int rz = 1;
    for (int i = 40; i < 52; i++) {
        if (rej[B + i] != 0) {
            rz = 0;
        }
    }
    CHECK(rz, "rejoin op 0x02 leaves body[40:52] zero");

    /* Nothing else may move. Assert the changed OFFSETS, not a count: the
     * specimen's own values happen to contain zero bytes (body[21], body[28]
     * and body[35] are 0x00 in the reference), so a count would silently
     * depend on which cluster the founding time came from. */
    int outside = 0;
    for (int i = 14; i < SCS_MEMBER_FRAME_LEN; i++) {
        if (first[i] == rej[i]) {
            continue;
        }
        int off = i - B;
        if (!((off >= 20 && off < 24) || (off >= 28 && off < 36) ||
              (off >= 36 && off < 40))) {
            outside = 1;
        }
    }
    CHECK(!outside,
          "rejoin form touches ONLY body[20:24], body[28:36] and body[36:40]");
}

/*
 * vms-e4b: op 0x0f ECHOES body[18]; every other allowlisted cat-0x01 opcode
 * FORCES it to 1.
 *
 * This is the only difference between the two recipes and it is invisible in
 * most specimens, because the requests that reach us usually carry body[18]=0
 * anyway -- forcing and echoing then produce the same bytes. It only separates
 * when the request carries a 1, which is exactly the specimen that made an
 * earlier session generalise the wrong way. So the test drives BOTH values
 * through BOTH recipes; anything less cannot tell them apart.
 */
static void test_op0f_echoes_response_marker(void)
{
    struct scs_member_params mp;
    memset(&mp, 0, sizeof(mp));
    memcpy(mp.dst_mac, vax1_logical, 6);
    memcpy(mp.src_mac, vax2_logical, 6);
    memcpy(mp.src_logical, vax2_logical, 6);
    memcpy(mp.peer_logical, vax1_logical, 6);
    mp.remote_conid = VAX1_VC_CONID;
    mp.local_conid = VAX2_VC_CONID;
    mp.incarnation = 1;

    static const uint8_t marker_values[2] = { 0x00, 0x01 };
    for (int i = 0; i < 2; i++) {
        uint8_t req[SCS_MEMBER_FRAME_LEN], out[SCS_MEMBER_FRAME_LEN];

        /* op 0x0f: the marker is echoed, whatever it was. */
        make_frame(req, golden_op03_req);
        req[72 + 9] = SCS_MEMBER_OP_0F;
        req[72 + SCS_MEMBER_RESP_MARK_BODYOFF] = marker_values[i];
        CHECK(scs_member_build_response(&mp, req, sizeof(req), out) == 0,
              "build_response op0f ok");
        CHECK(out[72 + 9] == SCS_MEMBER_OP_0F, "op0f response echoes opcode 0x0f");
        CHECK(out[72 + 8] == (SCS_MEMBER_CAT_CONFIG | SCS_MEMBER_RESPONSE_BIT),
              "op0f response sets the response bit");
        CHECK(out[72 + SCS_MEMBER_RESP_MARK_BODYOFF] == marker_values[i],
              "op0f ECHOES body[18] -- never forces it to 1");

        /* op 0x03, same input: the marker IS forced. */
        make_frame(req, golden_op03_req);
        req[72 + SCS_MEMBER_RESP_MARK_BODYOFF] = marker_values[i];
        CHECK(scs_member_build_response(&mp, req, sizeof(req), out) == 0,
              "build_response op03 ok (marker variant)");
        CHECK(out[72 + SCS_MEMBER_RESP_MARK_BODYOFF] == 0x01,
              "op03 FORCES body[18]=1 regardless of the request");
    }

    /* op 0x0d in category 0x01 is the class-0x04 self-departure open and takes
     * the ordinary forcing recipe. The identically-numbered cat-0x02 op 0x0d is
     * the DLM rebuild record and must NOT come through here -- it has its own
     * builder, and applying this transform to it corrupts the lock resource
     * name (that is the LOCKMGRERR bugcheck). Assert the cat-0x01 side; the DLM
     * side is covered by test_dlm_rebuild_response(). */
    uint8_t reqd[SCS_MEMBER_FRAME_LEN], outd[SCS_MEMBER_FRAME_LEN];
    make_frame(reqd, golden_op03_req);
    reqd[72 + 9] = SCS_MEMBER_OP_DEPART;
    reqd[72 + SCS_MEMBER_RESP_MARK_BODYOFF] = 0x00;
    reqd[72 + SCS_MEMBER_ROLE_BODYOFF] = SCS_MEMBER_ROLE_XITION;
    reqd[72 + SCS_MEMBER_CLASS_BODYOFF] = SCS_MEMBER_CLASS_DEPART;
    CHECK(scs_member_build_response(&mp, reqd, sizeof(reqd), outd) == 0,
          "build_response cat-0x01 op0d ok");
    CHECK(outd[72 + 9] == SCS_MEMBER_OP_DEPART, "op0d response echoes opcode 0x0d");
    CHECK(outd[72 + SCS_MEMBER_RESP_MARK_BODYOFF] == 0x01,
          "cat-0x01 op0d forces body[18]=1 (same recipe as 0x03/0x05/0x08/0x09)");
    CHECK(outd[72 + SCS_MEMBER_ROLE_BODYOFF] == SCS_MEMBER_ROLE_XITION &&
          outd[72 + SCS_MEMBER_CLASS_BODYOFF] == SCS_MEMBER_CLASS_DEPART,
          "op0d response leaves the role slot and class untouched");
}

/*
 * vms-ab1 (spec 4(O.29)): the class-0x04 op-0x0d SELF-DEPARTURE open OVMX emits
 * on a clean leave. It must be category 0x01 opcode 0x0d, role slot 0x40, class
 * 0x04, carry the epoch we were given, and -- critically -- carry NO
 * node-parameter body (that is the vms-760 crash class): everything past the
 * class byte is zero. This is what the inbound scsd.c class-0x04 handler already
 * accepts (GROUNDED 3/3), now emitted.
 */
static void test_build_depart_self_departure_open(void)
{
    struct scs_member_params mp;
    joiner_params(&mp, 20, 21, 5, 3);
    mp.txn = 0x0044;
    mp.checksum = 0x0007;
    uint8_t out[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_depart(&mp, 0x0000000Bu, out) == 0, "build_depart ok");
    const uint8_t *body = out + 72;
    CHECK(body[8] == SCS_MEMBER_CAT_CONFIG, "depart is category 0x01");
    CHECK(body[9] == SCS_MEMBER_OP_DEPART, "depart is opcode 0x0d");
    CHECK(body[SCS_MEMBER_ROLE_BODYOFF] == SCS_MEMBER_ROLE_XITION,
          "depart role slot body[16] == 0x40 (ROLE_XITION)");
    CHECK(body[SCS_MEMBER_CLASS_BODYOFF] == SCS_MEMBER_CLASS_DEPART,
          "depart class body[17] == 0x04 (CLASS_DEPART)");
    uint32_t epoch = (uint32_t)body[SCS_MEMBER_EPOCH_BODYOFF] |
                     ((uint32_t)body[SCS_MEMBER_EPOCH_BODYOFF + 1] << 8) |
                     ((uint32_t)body[SCS_MEMBER_EPOCH_BODYOFF + 2] << 16) |
                     ((uint32_t)body[SCS_MEMBER_EPOCH_BODYOFF + 3] << 24);
    CHECK(epoch == 0x0000000Bu, "depart carries the epoch it was given (body[12:16])");
    CHECK(out[30] == SCS_MEMBER_MSGTYPE && out[31] == SCS_MEMBER_FORMAT,
          "depart frame keeps the SCS envelope 0x4b/0x13");
    /* NO node-parameter body: the fatal-frame regression guard. Everything from
     * body[18] (past the epoch/role/class) to the end of the body is zero. */
    int nonzero = 0;
    for (int i = 18; i < SCS_MEMBER_SCA_LEN - SCS_MEMBER_BODY_OFF; i++) {
        if (body[i] != 0) { nonzero = 1; break; }
    }
    CHECK(nonzero == 0,
          "depart carries NO node-parameter body past the header/epoch/class"
          " (the vms-760 crash class); body[18:] must be all zero");
}

/*
 * vms-760: the cat-0x06 CLOSE must be built FRESH, never from the op-0x01 PARAMS
 * template. The PARAMS-derived version made body[10:132] byte-identical to our
 * own PARAMS body and killed two real VAXes (VAX3 INCONSTATE, VAX1 INVEXCEPTN,
 * each within 0.3 ms of receiving ours). These assertions are the regression
 * guard: this frame is fatal to other machines when it is wrong.
 */
static void test_close_is_not_the_params_body(void)
{
    struct scs_member_params mp;
    uint8_t req[SCS_MEMBER_FRAME_LEN];
    uint8_t out[SCS_MEMBER_FRAME_LEN];
    uint8_t params[SCS_MEMBER_FRAME_LEN];

    memset(&mp, 0, sizeof(mp));
    mp.sysap_send_msg = 7;
    mp.sysap_ack_msg = 9;

    /* A membership-close request: body[48:52] = 00 01 04 00, no ASCII name. */
    memset(req, 0, sizeof(req));
    req[14] = (uint8_t)((SCS_MEMBER_SCA_LEN - 2) & 0xff);
    req[15] = (uint8_t)(((SCS_MEMBER_SCA_LEN - 2) >> 8) & 0xff);
    req[72 + 4] = 0x11; req[72 + 5] = 0x22;   /* txn      */
    req[72 + 6] = 0x33; req[72 + 7] = 0x44;   /* checksum */
    req[72 + 8] = SCS_MEMBER_CAT_MEMBERSHIP;
    req[72 + 9] = 0x00;
    req[72 + 48] = 0x00; req[72 + 49] = 0x01;
    req[72 + 50] = 0x04; req[72 + 51] = 0x00;

    CHECK(scs_member_build_token_response(&mp, req, sizeof(req), out) == 0,
          "close: builder accepts a well-formed request");

    const uint8_t *body = out + 72;
    CHECK(body[8] == (SCS_MEMBER_CAT_MEMBERSHIP | SCS_MEMBER_RESPONSE_BIT),
          "close: category carries the response bit (0x86)");
    CHECK(body[4] == 0x11 && body[5] == 0x22 && body[6] == 0x33 && body[7] == 0x44,
          "close: (txn, checksum) carried verbatim -- never derived");

    /* THE regression: the close must NOT be our PARAMS body. */
    CHECK(scs_member_build_params(&mp, params) == 0, "close: params built");
    CHECK(memcmp(body + 10, params + 72 + 10, 122) != 0,
          "close: body[10:132] is NOT byte-identical to our op-0x01 PARAMS body");

    /* The live VMS time. A replayed constant here is a prime bugcheck suspect;
     * assert it lands in the cluster's era rather than 26 years adrift. The
     * reference peers carry a high longword of ~0x00bc03xx. */
    uint32_t t_hi = (uint32_t)body[68] | ((uint32_t)body[69] << 8) |
                    ((uint32_t)body[70] << 16) | ((uint32_t)body[71] << 24);
    CHECK(t_hi >= 0x00bc0000u && t_hi < 0x00bd0000u,
          "close: body[64:72] is a LIVE VMS time in the cluster's era");
    CHECK(t_hi != 0x009f570eu,
          "close: body[64:72] is not the old replayed PARAMS timestamp");

    /* Grounded scalars of variant A (ref f671 -> f673). */
    CHECK(body[10] == 0x03 && body[11] == 0x00, "close: body[10:12] = 0x0003");
    CHECK(body[24] == 0x03 && body[25] == 0x00, "close: body[24:26] = 0x0003");
    CHECK(body[44] == 0x03, "close: body[44:48] = 3");
    CHECK(body[48] == 0x6e, "close: body[48:52] = 110");
    CHECK(memcmp(body + 88, "V7.3    ", 8) == 0, "close: version string");
    CHECK(body[82] == 0x2b, "close: body[82] = 0x2b");

    /* Spans the reference leaves empty must be empty. */
    for (int i = 12; i < 24; i++) {
        CHECK(body[i] == 0, "close: body[12:24] zeroed (no peer handle echoed)");
    }
    for (int i = 96; i < 132; i++) {
        CHECK(body[i] == 0, "close: body[96:132] zeroed");
    }
}

/* The resource variant takes a DIFFERENT response -- body[24]=0x04, else zero.
 * Using variant A here would repeat the over-generalisation that crashed the
 * cluster, one category down. */
static void test_close_resource_variant(void)
{
    struct scs_member_params mp;
    uint8_t req[SCS_MEMBER_FRAME_LEN];
    uint8_t out[SCS_MEMBER_FRAME_LEN];

    memset(&mp, 0, sizeof(mp));
    memset(req, 0, sizeof(req));
    req[14] = (uint8_t)((SCS_MEMBER_SCA_LEN - 2) & 0xff);
    req[15] = (uint8_t)(((SCS_MEMBER_SCA_LEN - 2) >> 8) & 0xff);
    req[72 + 8] = SCS_MEMBER_CAT_MEMBERSHIP;
    memcpy(req + 72 + 48, "DTI$SYST", 8);

    CHECK(scs_member_close_is_resource(req + 72) == 1,
          "close: an ASCII resource name is detected");
    CHECK(scs_member_build_token_response(&mp, req, sizeof(req), out) == 0,
          "close: resource variant builds");

    const uint8_t *body = out + 72;
    CHECK(body[24] == 0x04, "close/resource: body[24] = 0x04");
    CHECK(body[10] == 0 && body[11] == 0,
          "close/resource: does NOT carry variant A's body[10:12]");
    for (int i = 64; i < 96; i++) {
        CHECK(body[i] == 0, "close/resource: no parameter block or timestamp");
    }
}

/*
 * vms-760: the cat-0x02 op-0x0d DLM rebuild response. A verbatim echo plus
 * body[34]=0xf9 -- and it must NOT take the cat-0x01 body[18]/body[55]
 * mutations, which land in the L1 region and the 8th byte of the lock RESOURCE
 * NAME. Applying them corrupted every record we answered and gave VAX1 and VAX3
 * a fatal LOCKMGRERR. This test is the guard on that.
 */
static void test_dlm_rebuild_response(void)
{
    struct scs_member_params mp;
    uint8_t req[SCS_MEMBER_FRAME_LEN];
    uint8_t out[SCS_MEMBER_FRAME_LEN];

    memset(&mp, 0, sizeof(mp));
    mp.sysap_send_msg = 5;
    mp.sysap_ack_msg = 6;

    memset(req, 0, sizeof(req));
    req[14] = (uint8_t)((SCS_MEMBER_SCA_LEN - 2) & 0xff);
    req[15] = (uint8_t)(((SCS_MEMBER_SCA_LEN - 2) >> 8) & 0xff);
    uint8_t *rb = req + 72;
    rb[4] = 0xaa; rb[5] = 0xbb;                 /* txn      */
    rb[6] = 0xcc; rb[7] = 0xdd;                 /* checksum */
    rb[8] = SCS_MEMBER_CAT_DLM;
    rb[9] = SCS_MEMBER_OP_DLM_REBUILD;
    rb[12] = 0x01; rb[14] = 0x03;               /* the two invariants */
    rb[18] = 0x5a;                              /* L1 payload byte  */
    rb[34] = 0x72;                              /* request-side stamp */
    rb[47] = 20;                                /* resource-name length */
    memcpy(rb + 48, "CACHE$cmSYSDSK1     ", 20);

    CHECK(scs_member_build_dlm_response(&mp, req, sizeof(req), out) == 0,
          "dlm: builder accepts a well-formed op-0x0d record");

    const uint8_t *ob = out + 72;
    CHECK(ob[8] == (SCS_MEMBER_CAT_DLM | SCS_MEMBER_RESPONSE_BIT),
          "dlm: category carries the response bit (0x82)");
    CHECK(ob[9] == SCS_MEMBER_OP_DLM_REBUILD, "dlm: opcode echoed");
    CHECK(ob[4] == 0xaa && ob[5] == 0xbb && ob[6] == 0xcc && ob[7] == 0xdd,
          "dlm: (txn, checksum) echoed verbatim");
    CHECK(ob[SCS_MEMBER_DLM_RESULT_BODYOFF] == SCS_MEMBER_DLM_RESULT_0D,
          "dlm: body[34] stamped 0xf9 unconditionally");

    /* The two mutations that killed real VAXes must NOT be applied. */
    CHECK(ob[18] == 0x5a, "dlm: body[18] NOT mutated (cat-0x01 only)");
    CHECK(ob[55] == req[72 + 55],
          "dlm: body[55] NOT zeroed -- it is the 8th byte of the resource name");

    /* The resource name must survive intact -- this is the whole ballgame. */
    CHECK(ob[47] == 20, "dlm: resource-name length echoed");
    CHECK(memcmp(ob + 48, "CACHE$cmSYSDSK1     ", 20) == 0,
          "dlm: lock resource name echoed byte-for-byte (corruption = LOCKMGRERR)");
    CHECK(ob[12] == 0x01 && ob[14] == 0x03, "dlm: body[12:16] invariants echoed");

    /* Envelope: our own counters, everything else verbatim. */
    CHECK(ob[0] == 5 && ob[2] == 6, "dlm: own send#/ack# substituted");
    CHECK(scs_member_build_dlm_response(&mp, req, 4, out) == -1,
          "dlm: short frame rejected");
}

/*
 * vms-e81: op 0x01 has a JOINER form and a MEMBER form, and OVMX shipped the
 * joiner form for both. The two differ in seven fields; a test that only checks
 * VOTES cannot tell them apart, which is how this survived to a live cluster.
 */
static void test_params_member_vs_joiner_form(void)
{
    struct scs_member_params mp;
    memset(&mp, 0, sizeof(mp));
    memcpy(mp.dst_mac, vax1_logical, 6);
    memcpy(mp.src_mac, vax2_logical, 6);
    memcpy(mp.src_logical, vax2_logical, 6);
    memcpy(mp.peer_logical, vax1_logical, 6);
    mp.remote_conid = VAX1_VC_CONID;
    mp.local_conid = VAX2_VC_CONID;
    mp.votes = SCS_MEMBER_VOTES_NONVOTING;

    /* JOINER form -- the default, and byte-for-byte the captured template. */
    uint8_t j[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_params(&mp, j) == 0, "build_params (joiner form) ok");
    const uint8_t *jb = j + 72;
    CHECK(jb[12] == 0x00, "joiner: body[12] == 0x00");
    CHECK(jb[82] == 0x2a, "joiner: body[82] == 0x2a");
    CHECK(jb[18] == 0 && jb[19] == 0, "joiner: member count == 0");
    CHECK(jb[28] == 0 && jb[35] == 0, "joiner: cluster-formation time is zero");
    CHECK(jb[36] == 0 && jb[43] == 0, "joiner: last-transition time is zero");
    /* The 2001-01-01 sentinel the captured joiner carries at body[64:72]. */
    static const uint8_t sentinel[8] = {0x00,0x80,0x4a,0x3f,0x0e,0x57,0x9f,0x00};
    CHECK(memcmp(jb + 64, sentinel, 8) == 0,
          "joiner: body[64:72] is the captured 2001-01-01 sentinel");

    /* MEMBER form. */
    mp.is_member = 1;
    mp.member_count = 3;
    mp.cluster_formed  = 0x00bc0419100a3040ULL;
    mp.last_transition = 0x00bc041987a2f2e0ULL;
    mp.own_admission   = 0x00bc04198899aabbULL;
    uint8_t m[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_params(&mp, m) == 0, "build_params (member form) ok");
    const uint8_t *mb = m + 72;
    CHECK(mb[12] == 0x21, "member: body[12] == 0x21");
    CHECK(mb[82] == 0x2b, "member: body[82] == 0x2b");
    CHECK((uint16_t)(mb[18] | (mb[19] << 8)) == 3, "member: member count == 3");
    CHECK((uint32_t)(mb[44] | (mb[45] << 8) | (mb[46] << 16) | ((uint32_t)mb[47] << 24)) == 4,
          "member: state-seq body[44:48] == member count + 1");
    CHECK(mb[28] == 0x40 && mb[29] == 0x30 && mb[31] == 0x10 &&
          mb[32] == 0x19 && mb[34] == 0xbc && mb[35] == 0x00,
          "member: cluster-formation quadword laid down little-endian");
    CHECK(mb[36] == 0xe0 && mb[43] == 0x00,
          "member: last-transition quadword laid down little-endian");
    CHECK(memcmp(mb + 64, sentinel, 8) != 0,
          "member: body[64:72] is NOT the joiner sentinel any more");
    CHECK(mb[64] == 0xbb && mb[71] == 0x00, "member: own-admission quadword laid down");

    /* VOTES and the version token must survive both forms untouched. */
    CHECK((uint16_t)(jb[22] | (jb[23] << 8)) == 0 &&
          (uint16_t)(mb[22] | (mb[23] << 8)) == 0, "VOTES stays 0 in both forms");
    CHECK(memcmp(jb + 88, "V7.3    ", 8) == 0 && memcmp(mb + 88, "V7.3    ", 8) == 0,
          "version token 'V7.3    ' byte-exact in both forms");
}

/* vms-db20: the op 0x0d SCS$DIRECTORY self-registration a real joiner originates
 * toward the coordinator. ORACLE = the SYSAP body of the captured joiner frame in
 * vax3-2to3-established-join (idx 698, VAX3 sysid 1027 -> VAX2 coordinator,
 * t=34.813s). The joiner MINTS body[4:8] (the coordinator echoes them opaquely --
 * proven by its verbatim cat-0x82 op-0x0d reply), so the test reproduces the
 * captured frame given the same minted (txn, counter) and asserts a NULL value
 * block (no held-lock claim, INV-6). */
static const uint8_t vax3_logical[6] = {0xaa,0x00,0x04,0x00,0x03,0x04};

static const uint8_t golden_dlm_selfreg_body[64] = {
    0x64,0x00,0x13,0x01,0x03,0x00,0xf9,0x07,0x02,0x0d,0x52,0x5f,0x01,0x00,0x03,0x00,
    0x10,0x02,0x01,0x00,0x53,0x43,0x53,0x24,0x44,0x49,0x52,0x45,0x43,0x54,0x4f,0x52,
    0x59,0x20,0x20,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x10,
    0x53,0x59,0x53,0x24,0x53,0x59,0x53,0x5f,0x49,0x44,0x03,0x04,0x00,0x00,0x00,0x00,
};

static void test_dlm_selfreg_byte_exact(void)
{
    struct scs_member_params mp;
    joiner_params(&mp, 0, 0, 0x0064, 0x0113);
    memcpy(mp.src_logical, vax3_logical, 6); /* joiner VAX3 -> sysid 03 04 */
    mp.txn = 0x0003;      /* the minted directory-tree tag the reference carried */
    mp.checksum = 0x07f9; /* the minted per-VC DLM counter the reference carried */

    uint8_t out[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_dlm_selfreg(&mp, out) == 0, "build_dlm_selfreg ok");

    const uint8_t *body = out + 14 + SCS_MEMBER_BODY_OFF; /* SCA[58] = abs 72 */
    CHECK(memcmp(body, golden_dlm_selfreg_body, sizeof golden_dlm_selfreg_body) == 0,
          "dlm selfreg body[0:64] reproduces the captured joiner frame byte-for-byte");

    int lvb_nonzero = 0;
    for (int i = 64; i < (SCS_MEMBER_SCA_LEN - SCS_MEMBER_BODY_OFF); i++) {
        if (body[i] != 0) { lvb_nonzero = 1; }
    }
    CHECK(!lvb_nonzero, "dlm selfreg value block is null (asserts no held lock, INV-6)");

    CHECK(body[8] == 0x02, "cat 0x02 (DLM request)");
    CHECK(body[9] == SCS_MEMBER_OP_DLM_REBUILD, "op 0x0d");
    CHECK(body[4] == 0x03 && body[5] == 0x00, "body[4:6] = minted dir-tree tag (echoed, not computed)");
    CHECK(body[6] == 0xf9 && body[7] == 0x07, "body[6:8] = minted per-VC counter (NOT a CRC)");

    /* body[58:60] tracks the node's OWN sysid from the source logical. */
    struct scs_member_params mp2;
    joiner_params(&mp2, 0, 0, 1, 0); /* src_logical = vax2 -> sysid 02 04 */
    mp2.txn = 0x0003; mp2.checksum = 0x0001;
    uint8_t out2[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_dlm_selfreg(&mp2, out2) == 0, "build_dlm_selfreg vax2 ok");
    const uint8_t *body2 = out2 + 14 + SCS_MEMBER_BODY_OFF;
    CHECK(body2[58] == 0x02 && body2[59] == 0x04,
          "sysid at body[58:60] follows the source logical (VAX2 -> 02 04)");
    /* The rest of the record is identical regardless of node (constants). */
    CHECK(memcmp(body2 + 8, golden_dlm_selfreg_body + 8, 50) == 0,
          "body[8:58] constants are node-independent");
}

static void test_dlm_selfreg_null_guards(void)
{
    struct scs_member_params mp;
    uint8_t out[SCS_MEMBER_FRAME_LEN];
    joiner_params(&mp, 0, 0, 1, 0);
    CHECK(scs_member_build_dlm_selfreg(NULL, out) == -1, "build_dlm_selfreg NULL p");
    CHECK(scs_member_build_dlm_selfreg(&mp, NULL) == -1, "build_dlm_selfreg NULL out");
}

/*
 * vms-cn3: the rebuild-COMPLETION pair (op-04 + op-03 COMMIT) OVMX drives to the
 * coordinator must be HONEST BY CONSTRUCTION -- no named resource, no held-lock
 * handle. OVMX holds no persistent cluster lock, so the completion asserts
 * "done, holding nothing". This pins that guarantee: resname@48 zeroed, per-lock
 * handles@20:28 zeroed, mode@30 zeroed (NL). If any of those ever carries a value
 * it would be claiming a lock OVMX cannot honestly back -- a fabrication, caught
 * here rather than on the wire.
 */
static void test_dlm_completion_holds_nothing(void)
{
    struct scs_member_params mp;
    joiner_params(&mp, 0, 0, 0x0064, 0x0113);
    mp.txn = 0x0003;
    mp.checksum = 0x0100;

    uint32_t lkid = 0x07f70004u;   /* OVMX's own real per-lock handle */
    uint8_t op04[SCS_MEMBER_FRAME_LEN], op03[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_dlm_op04(&mp, lkid, op04) == 0, "build_dlm_op04 ok");
    CHECK(scs_member_build_dlm_commit(&mp, lkid, op03) == 0, "build_dlm_commit ok");

    const uint8_t *b4 = op04 + 72;
    const uint8_t *b3 = op03 + 72;

    CHECK(b4[8] == 0x02 && b4[9] == 0x04, "op-04: cat 0x02 op 0x04");
    CHECK(b3[8] == 0x02 && b3[9] == 0x03, "op-03: cat 0x02 op 0x03 (COMMIT)");

    /* Layer 3: op-04/op-03 carry OVMX's OWN real per-lock handle @[20:24]. */
    CHECK((uint32_t)(b4[20] | (b4[21] << 8) | (b4[22] << 16) | ((uint32_t)b4[23] << 24)) == lkid,
          "op-04: OVMX's real lock handle at body[20:24]");
    CHECK((uint32_t)(b3[20] | (b3[21] << 8) | (b3[22] << 16) | ((uint32_t)b3[23] << 24)) == lkid,
          "op-03: OVMX's real lock handle at body[20:24]");

    /* THE HONESTY GUARDRAIL -- no named resource, NL mode, and the ungrounded
     * second handle word @[24:28] stays ZERO (never invent VAX3's value). */
    for (int i = 48; i < 64; i++) {
        CHECK(b4[i] == 0 && b3[i] == 0, "completion: resname@48 is ZEROED (op-03 commits by handle)");
    }
    for (int i = 24; i < 28; i++) {
        CHECK(b4[i] == 0 && b3[i] == 0, "completion: 2nd handle word body[24:28] ZERO (ungrounded, INV-6)");
    }
    CHECK(b4[30] == 0 && b3[30] == 0, "completion: mode@30 is 0x00 (NL -- the mode OVMX holds)");

    /* The SYSAP envelope + minted per-VC fields are laid down (real send). */
    CHECK((uint16_t)(b4[4] | (b4[5] << 8)) == 0x0003, "op-04: dir-tree tag minted");
    CHECK((uint16_t)(b4[6] | (b4[7] << 8)) == 0x0100, "op-04: per-VC counter minted");

    /* lkid == 0 gives the content-free frame (the null case). */
    uint8_t z[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_dlm_op04(&mp, 0, z) == 0 &&
          (z + 72)[20] == 0 && (z + 72)[21] == 0 && (z + 72)[22] == 0 && (z + 72)[23] == 0,
          "op-04 with lkid=0 leaves body[20:24] zero (content-free null case)");

    CHECK(scs_member_build_dlm_op04(NULL, lkid, op04) == -1, "build_dlm_op04 NULL p");
    CHECK(scs_member_build_dlm_commit(&mp, lkid, NULL) == -1, "build_dlm_commit NULL out");
}

/*
 * vms-74f (Layer 3): the op-01 ENQ that REGISTERS one of OVMX's REAL standing
 * locks to the coordinator must carry OVMX's OWN real values -- its real lock
 * handle (req_lkid@[4:8], from the accessor), the real coordinator csid
 * (mst_csid@[20:24]), its real resource name, NL mode -- and must NOT invent the
 * ungrounded per-lock lock-mgmt fields (body[24:30] zero). This pins that.
 */
static void test_dlm_reg_enq_carries_real_values(void)
{
    struct scs_member_params mp;
    joiner_params(&mp, 0, 0, 0x0064, 0x0113);
    mp.member_count = 3;

    const char *res = "F11B$vOVMXSYS";
    uint8_t nl = (uint8_t)strlen(res);
    struct scs_dlm_reg_fields f;
    memset(&f, 0, sizeof(f));
    f.req_lkid = 0x00000001u;   /* OVMX's own lock handle */
    f.dir_csid = 0x00010003u;   /* OVMX's own directory-master csid (its encoding) */
    f.lock_id  = 0x00000001u;   /* OVMX's per-lock id */
    f.flags    = 0x007du;       /* lock flags */
    f.mode     = 0x00u;         /* NL */
    f.lockmgmt = 0x00010001u;   /* count word */

    uint8_t out[SCS_MEMBER_FRAME_LEN];
    CHECK(scs_member_build_dlm_reg_enq(&mp, res, nl, &f, out) == 0, "build_dlm_reg_enq ok");
    const uint8_t *b = out + 72;

#define RD32(o) ((uint32_t)(b[o] | (b[(o)+1] << 8) | (b[(o)+2] << 16) | ((uint32_t)b[(o)+3] << 24)))
    CHECK(RD32(4)  == f.req_lkid,  "op-01: OVMX's real req_lkid at body[4:8]");
    CHECK(b[8] == 0x02 && b[9] == 0x01, "op-01: cat 0x02 op 0x01 (ENQ)");
    CHECK(RD32(16) == 0,           "op-01: req_csid body[16:20] is 0 (as every granted ref ENQ)");
    CHECK(RD32(20) == f.dir_csid,  "op-01: OVMX's own dir_csid at body[20:24] (the measured decider)");
    CHECK(RD32(24) == f.lock_id,   "op-01: OVMX's real per-lock id at body[24:28] (non-zero, as every granted ENQ)");
    CHECK((uint16_t)(b[28] | (b[29] << 8)) == f.flags, "op-01: lock flags at body[28:30]");
    CHECK(b[30] == f.mode,         "op-01: mode@30 (NL, the mode OVMX holds)");
    CHECK(RD32(32) == f.lockmgmt,  "op-01: lock-mgmt count word at body[32:36]");
    CHECK(b[47] == nl && memcmp(b + 48, res, nl) == 0, "op-01: OVMX's real resource name at body[48]");
#undef RD32

    CHECK(scs_member_build_dlm_reg_enq(&mp, NULL, nl, &f, out) == -1, "build_dlm_reg_enq NULL resname");
    CHECK(scs_member_build_dlm_reg_enq(&mp, res, nl, NULL, out) == -1, "build_dlm_reg_enq NULL fields");
    CHECK(scs_member_build_dlm_reg_enq(&mp, res, 0, &f, out) == -1, "build_dlm_reg_enq zero namelen guarded");
    CHECK(scs_member_build_dlm_reg_enq(&mp, res, 32, &f, out) == -1, "build_dlm_reg_enq namelen > 31 guarded");
}

/*
 * The coordinator's cat-0x82 op-01 GRANT for our standing-lock registration.
 * scs_member_parse stores the RAW category byte (body[8]) -- so a response frame
 * carries the 0x80 bit IN v.category, and v.is_response is derived from it. Any
 * consumer that classifies category MUST mask with & 0x7f: a bare
 * `v.category == SCS_MEMBER_CAT_DLM` is UNSATISFIABLE for a response (0x82 != 0x02).
 * This locks that contract -- the exact trap that silenced scsd.c's grant-detect
 * (cm_send_dlm_completion trigger, vms-74f): a GRANT that never fires the op-04/
 * op-03 completion dangles indistinguishably from a DROP.
 */
static void test_dlm_grant_response_needs_masking(void)
{
    uint8_t frame[SCS_MEMBER_FRAME_LEN];
    struct scs_member_view v;

    /* Start from a valid config frame, then stamp a cat-0x82 op-01 grant. */
    make_frame(frame, golden_op01);
    uint8_t *b = frame + 72;
    b[8] = (uint8_t)(SCS_MEMBER_CAT_DLM | SCS_MEMBER_RESPONSE_BIT); /* 0x82 */
    b[9] = 0x01;

    CHECK(scs_member_parse(frame, sizeof(frame), &v) == 0, "parse cat-82 op-01 grant ok");
    CHECK(v.is_response == 1, "grant: is_response set from the 0x80 bit");
    CHECK(v.opcode == 0x01, "grant: opcode 0x01");
    CHECK(v.category == 0x82, "grant: RAW category retains the response bit (0x82)");
    CHECK(v.category != SCS_MEMBER_CAT_DLM,
          "grant: a BARE `category == CAT_DLM` FAILS on a response (the silenced-trigger trap)");
    CHECK((v.category & 0x7f) == SCS_MEMBER_CAT_DLM,
          "grant: the MASKED category is DLM -- what the grant-detect must test");
}

int main(void)
{
    test_op14_byte_exact();
    test_op01_byte_exact_and_votes();
    test_op02_byte_exact();
    test_default_model_is_ovmx();
    test_parse_classification();
    test_response_echoes_real_checksum();
    test_both_conids_p235();
    test_null_guards();
    /* vms-578: worktree-760 test functions, registered here too. */
    test_dlm_rebuild_response();
    test_close_is_not_the_params_body();
    test_close_resource_variant();
    test_op02_residue_fields_are_zero();
    test_op02_rejoin_form();
    test_op0f_echoes_response_marker();
    test_build_depart_self_departure_open();
    test_params_member_vs_joiner_form();
    test_dlm_selfreg_byte_exact();
    test_dlm_selfreg_null_guards();
    test_dlm_completion_holds_nothing();
    test_dlm_reg_enq_carries_real_values();
    test_dlm_grant_response_needs_masking();

    if (failures == 0) {
        printf("test_scs_member: ALL PASSED\n");
        return 0;
    }
    fprintf(stderr, "test_scs_member: %d FAILURE(S)\n", failures);
    return 1;
}
