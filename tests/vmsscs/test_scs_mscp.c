/*
 * test_scs_mscp.c - targeted unit tests for the MSCP-over-SCS disk-client
 * command builders/parser (vms-760).
 *
 * ORACLE. Every expected byte array below is the byte-exact SCA content of a
 * real frame from af2-firsttimer-established-20260728.pcap (the clean-room
 * observation of a first-timer joining an established VAX1), the ground-truth
 * reference cluster wire. The builders are validated by reproducing those
 * captured joiner MSCP commands byte-for-byte; the parser is validated against
 * the captured VAX1 END responses (SCC-END 0x84 / GUS-END 0x83), proving OVMX
 * reads the echoed correlation token + returned unit/status rather than guessing.
 */
#include "scs_mscp.h"

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

/* Golden joiner SCC command SCA content (94 bytes) -- af2 rel~143.89574. */
static const uint8_t golden_scc[94] = {
    0x5c,0x00,0xaa,0x00,0x04,0x00,0x01,0x04,0x01,0x00,0xaa,0x00,
    0x04,0x00,0x1a,0x04,0x4b,0x13,0x18,0x00,0x19,0x00,0x01,0x00,
    0x12,0x00,0x18,0x00,0x00,0x00,0x19,0x00,0x00,0x00,0x18,0x00,
    0x00,0x00,0x01,0x00,0x00,0x02,0x32,0x00,0x04,0x00,0x0a,0x00,
    0x01,0x00,0x0a,0x00,0x54,0x35,0x08,0x00,0xd2,0x8f,0x02,0x00,
    0xa3,0x81,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x00,0x00,
    0xd0,0x00,0x00,0x00,0x00,0x00,0xc0,0x0b,0x28,0x75,0x19,0x02,
    0xbc,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

/* Golden joiner GUS command SCA content (94 bytes) -- af2 rel~143.89732. */
static const uint8_t golden_gus[94] = {
    0x5c,0x00,0xaa,0x00,0x04,0x00,0x01,0x04,0x01,0x00,0xaa,0x00,
    0x04,0x00,0x1a,0x04,0x4b,0x13,0x1a,0x00,0x1b,0x00,0x01,0x00,
    0x12,0x00,0x1a,0x00,0x00,0x00,0x1b,0x00,0x00,0x00,0x1a,0x00,
    0x00,0x00,0x01,0x00,0x00,0x02,0x32,0x00,0x04,0x00,0x0a,0x00,
    0x01,0x00,0x0a,0x00,0x54,0x35,0x08,0x00,0xd2,0x8f,0x01,0x00,
    0xe2,0x7e,0x01,0x00,0x00,0x00,0x03,0x00,0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

/* Golden VAX1 SCC-END response SCA content (86 bytes) -- op 0x84, status 0. */
static const uint8_t golden_scc_end[86] = {
    0x54,0x00,0xaa,0x00,0x04,0x00,0x1a,0x04,0x01,0x00,0xaa,0x00,
    0x04,0x00,0x01,0x04,0x4b,0x13,0x19,0x00,0x19,0x00,0x01,0x00,
    0x12,0x00,0x19,0x00,0x00,0x00,0x19,0x00,0x00,0x00,0x19,0x00,
    0x00,0x00,0x01,0x00,0x00,0x02,0x2a,0x00,0x04,0x00,0x0a,0x00,
    0x01,0x00,0x08,0x00,0xd2,0x8f,0x0a,0x00,0x54,0x35,0x02,0x00,
    0xa3,0x81,0x02,0x00,0x00,0x00,0x84,0x00,0x00,0x00,0x00,0x00,
    0x04,0xa0,0x14,0x00,0x47,0x05,0x01,0x04,0x00,0x00,0x00,0x00,
    0x04,0x01,
};

/* Golden VAX1 GUS-END response SCA content (110 bytes) -- op 0x83, unit 0x4000,
 * status 0x0004 (UNIT AVAILABLE). */
static const uint8_t golden_gus_end[110] = {
    0x6c,0x00,0xaa,0x00,0x04,0x00,0x1a,0x04,0x01,0x00,0xaa,0x00,
    0x04,0x00,0x01,0x04,0x4b,0x13,0x1b,0x00,0x1b,0x00,0x01,0x00,
    0x12,0x00,0x1b,0x00,0x00,0x00,0x1b,0x00,0x00,0x00,0x1b,0x00,
    0x00,0x00,0x01,0x00,0x00,0x02,0x42,0x00,0x04,0x00,0x0a,0x00,
    0x01,0x00,0x08,0x00,0xd2,0x8f,0x0a,0x00,0x54,0x35,0x01,0x00,
    0xe2,0x7e,0x00,0x40,0x00,0x00,0x83,0x00,0x04,0x00,0x00,0x00,
    0x00,0x80,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,
    0xa1,0x12,0x5c,0x10,0x64,0x25,0x00,0x00,0x00,0x00,0x49,0x00,
    0x0d,0x00,0x01,0x00,0x00,0x00,0xb5,0x03,0x01,0x01,0x6e,0x00,
    0x31,0x42,
};

/* VAX1 (member) logical + OVMX/joiner logical for byte-exact reproduction. */
static const uint8_t vax1_logical[6]   = {0xaa,0x00,0x04,0x00,0x01,0x04};
static const uint8_t joiner_logical[6] = {0xaa,0x00,0x04,0x00,0x1a,0x04};

#define VAX1_MSCP_CONID   0x3554000au /* member's MSCP$DISK server handle (abs64) */
#define JOINER_MSCP_CONID 0x8fd20008u /* joiner's MSCP$DISK client handle (abs68) */

static void fill_params(struct scs_mscp_params *p, uint16_t recv_ack,
                        uint16_t send_seq, uint16_t class_token,
                        uint16_t msg_id, uint16_t unit)
{
    memset(p, 0, sizeof(*p));
    memcpy(p->dst_mac, vax1_logical, 6);   /* Ethernet header unused by SCA compare */
    memcpy(p->src_mac, joiner_logical, 6);
    memcpy(p->src_logical, joiner_logical, 6);
    memcpy(p->peer_logical, vax1_logical, 6);
    p->remote_conid = VAX1_MSCP_CONID;
    p->local_conid = JOINER_MSCP_CONID;
    p->recv_ack = recv_ack;
    p->send_seq = send_seq;
    p->incarnation = 1; /* fresh first-timer join -- matches the golden template */
    p->class_token = class_token;
    p->msg_id = msg_id;
    p->unit = unit;
}

static void test_scc_byte_exact(void)
{
    struct scs_mscp_params p;
    fill_params(&p, 24, 25, SCS_MSCP_SCC_CLASS, SCS_MSCP_SCC_MSGID0, 0);
    uint8_t out[SCS_MSCP_FRAME_LEN];
    CHECK(scs_mscp_build_scc(&p, out) == 0, "build_scc ok");
    CHECK(memcmp(out + 14, golden_scc, 94) == 0,
          "SCC reproduces golden af2 SCC command byte-exact");
    /* Spot-check the load-bearing MSCP fields. */
    const uint8_t *body = out + 72;
    CHECK(out[30] == SCS_MSCP_MSGTYPE && out[31] == SCS_MSCP_FORMAT,
          "SCC keeps SCS envelope 0x4b/0x13");
    CHECK(body[8] == SCS_MSCP_OP_SET_CTLR_CHAR, "SCC opcode 0x04");
    CHECK((uint16_t)(body[0] | (body[1] << 8)) == SCS_MSCP_SCC_CLASS,
          "SCC class token 0x0002");
    CHECK((uint16_t)(body[2] | (body[3] << 8)) == SCS_MSCP_SCC_MSGID0,
          "SCC message-id 0x81a3");
    CHECK((uint16_t)(body[4] | (body[5] << 8)) == 0, "SCC unit word 0 (controller)");
    CHECK((uint16_t)(body[10] | (body[11] << 8)) == 0, "SCC modifiers 0x0000");
}

static void test_gus_byte_exact(void)
{
    struct scs_mscp_params p;
    fill_params(&p, 26, 27, SCS_MSCP_GUS_CLASS, SCS_MSCP_GUS_MSGID0, 0x0001);
    uint8_t out[SCS_MSCP_FRAME_LEN];
    CHECK(scs_mscp_build_gus(&p, out) == 0, "build_gus ok");
    CHECK(memcmp(out + 14, golden_gus, 94) == 0,
          "GUS reproduces golden af2 GUS command byte-exact");
    const uint8_t *body = out + 72;
    CHECK(body[8] == SCS_MSCP_OP_GET_UNIT_STATUS, "GUS opcode 0x03");
    CHECK((uint16_t)(body[10] | (body[11] << 8)) == SCS_MSCP_MOD_NEXT_UNIT,
          "GUS carries NEXT-UNIT modifier 0x0001");
    CHECK((uint16_t)(body[4] | (body[5] << 8)) == 0x0001, "GUS unit word 0x0001");
}

/* The unit/msg-id substitution must change ONLY the intended body bytes. */
static void test_gus_unit_substitution(void)
{
    struct scs_mscp_params p;
    uint8_t base[SCS_MSCP_FRAME_LEN], out[SCS_MSCP_FRAME_LEN];
    fill_params(&p, 26, 27, SCS_MSCP_GUS_CLASS, SCS_MSCP_GUS_MSGID0, 0x0001);
    CHECK(scs_mscp_build_gus(&p, base) == 0, "build_gus base ok");

    /* cmd#2 of the enumeration: unit 0x4001, message-id incremented (echoed). */
    p.unit = 0x4001;
    p.msg_id = (uint16_t)(SCS_MSCP_GUS_MSGID0 + 1);
    CHECK(scs_mscp_build_gus(&p, out) == 0, "build_gus unit=0x4001 ok");
    const uint8_t *b = out + 72;
    CHECK((uint16_t)(b[4] | (b[5] << 8)) == 0x4001, "unit word advanced to 0x4001");
    CHECK((uint16_t)(b[2] | (b[3] << 8)) == (uint16_t)(SCS_MSCP_GUS_MSGID0 + 1),
          "message-id incremented");
    /* Exactly the unit word (body[4:6]=abs 76:78) and message-id (body[2:4]=abs
     * 74:76) differ between the two frames. */
    int diffs = 0;
    for (int i = 14; i < SCS_MSCP_FRAME_LEN; i++) {
        if (base[i] != out[i]) {
            diffs++;
            CHECK(i >= 74 && i < 78, "GUS unit/msg-id diff confined to body[2:6]");
        }
    }
    CHECK(diffs > 0 && diffs <= 4, "unit/msg-id change touches only body[2:6]");
}

static void make_frame(uint8_t *out, size_t frame_len, const uint8_t *sca,
                       size_t sca_len)
{
    memset(out, 0, frame_len);
    out[12] = 0x60;
    out[13] = 0x07;
    memcpy(out + 14, sca, sca_len);
}

static void test_parse_scc_end(void)
{
    uint8_t frame[14 + 86];
    make_frame(frame, sizeof(frame), golden_scc_end, 86);
    struct scs_mscp_view v;
    CHECK(scs_mscp_parse(frame, sizeof(frame), &v) == 0, "parse SCC-END ok");
    CHECK(v.msgtype == SCS_MSCP_MSGTYPE && v.format == SCS_MSCP_FORMAT,
          "SCC-END SCS envelope");
    CHECK(v.opcode == (SCS_MSCP_OP_SET_CTLR_CHAR | SCS_MSCP_END_BIT) && v.is_end,
          "SCC-END opcode 0x84 (END bit set)");
    CHECK(v.status == SCS_MSCP_ST_SUCCESS, "SCC-END status SUCCESS (0)");
    /* Correlation token echoed from the joiner's SCC command. */
    CHECK(v.class_token == SCS_MSCP_SCC_CLASS && v.msg_id == SCS_MSCP_SCC_MSGID0,
          "SCC-END echoes the joiner's command-reference token");
    CHECK(v.remote_conid == JOINER_MSCP_CONID && v.local_conid == VAX1_MSCP_CONID,
          "SCC-END Con.ID pair (remote=joiner client, local=VAX1 server)");
}

static void test_parse_gus_end(void)
{
    uint8_t frame[14 + 110];
    make_frame(frame, sizeof(frame), golden_gus_end, 110);
    struct scs_mscp_view v;
    CHECK(scs_mscp_parse(frame, sizeof(frame), &v) == 0, "parse GUS-END ok");
    CHECK(v.opcode == (SCS_MSCP_OP_GET_UNIT_STATUS | SCS_MSCP_END_BIT) && v.is_end,
          "GUS-END opcode 0x83 (END bit set)");
    CHECK(v.status == SCS_MSCP_ST_AVAILABLE, "GUS-END status UNIT AVAILABLE (4)");
    /* The returned unit-word drives the NEXT-UNIT enumeration (next = +1). */
    CHECK(v.unit == 0x4000, "GUS-END returns unit-word 0x4000");
    CHECK(v.msg_id == SCS_MSCP_GUS_MSGID0, "GUS-END echoes the GUS message-id");
}

static void test_null_guards(void)
{
    uint8_t out[SCS_MSCP_FRAME_LEN];
    struct scs_mscp_params p;
    memset(&p, 0, sizeof(p));
    CHECK(scs_mscp_build_scc(NULL, out) == -1, "build_scc NULL p");
    CHECK(scs_mscp_build_scc(&p, NULL) == -1, "build_scc NULL out");
    CHECK(scs_mscp_build_gus(NULL, out) == -1, "build_gus NULL p");
    struct scs_mscp_view v;
    CHECK(scs_mscp_parse(NULL, 108, &v) == -1, "parse NULL frame");
    uint8_t shortf[40] = {0};
    CHECK(scs_mscp_parse(shortf, sizeof(shortf), &v) == -1, "parse short frame");
}

int main(void)
{
    test_scc_byte_exact();
    test_gus_byte_exact();
    test_gus_unit_substitution();
    test_parse_scc_end();
    test_parse_gus_end();
    test_null_guards();

    if (failures == 0) {
        printf("test_scs_mscp: ALL PASSED\n");
        return 0;
    }
    fprintf(stderr, "test_scs_mscp: %d FAILURE(S)\n", failures);
    return 1;
}
