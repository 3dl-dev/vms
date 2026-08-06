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

/*
 * vms-4eb -- the 110-byte GUS-END class decoded field by field.
 *
 * ORACLE, both halves. The bytes are golden_gus_end[] above: the byte-exact SCA
 * content of a real VAX1 frame from af2-firsttimer-established-20260728.pcap.
 * The FIELD MAP is *MSCP Basic Disk Functions Manual* AA-L619A-TK v1.2 (public,
 * CLAUDE.md rule 8 -- the UDA50 doc kit manual, no distribution restriction;
 * the DEC-Confidential bitsavers MSCP 2.4.0 files are EXCLUDED and were not
 * read): Table A-7 "GET UNIT STATUS end message offsets", sec 6.12, Table A-3
 * (end flags), Table B-1/B-2 (status), sec 4.17 + Appendix C (media type id).
 *
 * WHY THIS IS HERE AND NOT ONLY IN THE PYTHON FIGURES GATE. The census in
 * tools/cluster/scs_type10_measure.py runs only where the host-only lab
 * captures are; this runs everywhere ctest does, on bytes that are IN GIT. It
 * is what stops the decode in spec sec 4(h)(1e) from being a claim nobody can
 * re-check on a machine without the capture library.
 *
 * All offsets are MSCP-body-relative, i.e. from SCA content[58].
 */
#define GUS_BODY (golden_gus_end + SCS_MSCP_BODY_OFF)

static uint16_t bu16(const uint8_t *b, int off)
{
    return (uint16_t)(b[off] | (b[off + 1] << 8));
}

static uint32_t bu32(const uint8_t *b, int off)
{
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8)
         | ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

/* AA-L619A-TK sec 4.17: D0 (bits 31-27) and D1 (26-22) are the device-type
 * name, A0..A2 (21-17, 16-12, 11-7) are one to three media-name characters
 * ("A" == 1, 0 == absent), N (bits 6-0) holds the two decimal digits. */
static void media_name(uint32_t v, char out[8])
{
    static const int shift[5] = {27, 22, 17, 12, 7};
    int i, n = 0;
    for (i = 0; i < 5; i++) {
        unsigned c = (v >> shift[i]) & 0x1Fu;
        if (c)
            out[n++] = (char)('A' + c - 1);
    }
    out[n++] = (char)('0' + (v & 0x7Fu) / 10);
    out[n++] = (char)('0' + (v & 0x7Fu) % 10);
    out[n] = '\0';
}

static void test_gus_end_field_map(void)
{
    const uint8_t *b = GUS_BODY;
    char name[8];

    /* Appendix C Table C-3 publishes ONE worked media-type value: RA80 =
     * hex 2564,1050. The decoder must reproduce it, or reading the captured
     * value with it proves nothing. This is the calibration, not a decoration. */
    media_name(0x25641050u, name);
    CHECK(strcmp(name, "DURA80") == 0,
          "media_name() reproduces Appendix C Table C-3's published RA80 value");

    /* --- generic end-message header, Table A-7 ------------------------- */
    CHECK(bu32(b, 0) == (((uint32_t)SCS_MSCP_GUS_MSGID0 << 16) | SCS_MSCP_GUS_CLASS),
          "P.CRF command reference number is the token echoed from the command");
    CHECK(bu16(b, 4) == 0x4000, "P.UNIT unit number 0x4000");
    CHECK(bu16(b, 6) == 0, "generic reserved [6:8] is zero (AA-L619A-TK sec 5.2)");
    CHECK(b[8] == (SCS_MSCP_OP_GET_UNIT_STATUS | SCS_MSCP_END_BIT),
          "P.OPCD endcode 0x83 == OP.GUS | OP.END (Table A-1)");
    CHECK(b[9] == 0x00,
          "P.FLGS end flags clear: no Bad Block Reported / Unreported / Error "
          "Log Generated (Table A-3)");
    CHECK(bu16(b, 10) == SCS_MSCP_ST_AVAILABLE,
          "P.STS status 0x0004 = Unit-Available, sub-code 0 (Table B-1/B-2)");

    /* --- GET UNIT STATUS specifics, Table A-7 + sec 6.12 --------------- */
    CHECK(bu16(b, 12) == 0x0000,
          "P.MLUN multi-unit code 0 (sec 6.12: low byte access path, high byte "
          "spindle id) -- unit 0x4000 is spindle 0");
    CHECK(bu16(b, 16) == 0 && bu16(b, 18) == 0,
          "GUS reserved [16:20] is zero");
    /* sec 6.12: a non-zero unit identifier is what makes the characteristics
     * valid. This frame reports a unit, so it must be non-zero. */
    CHECK(bu32(b, 20) != 0 || bu32(b, 24) != 0,
          "P.UNTI unit identifier non-zero, so sec 6.12 says the "
          "characteristics below are valid");
    CHECK(bu32(b, 20) == 0x00000002u && bu32(b, 24) == 0x12a10000u,
          "P.UNTI unit identifier 02 00 00 00 | 00 00 a1 12");
    media_name(bu32(b, 28), name);
    CHECK(strcmp(name, "DURA92") == 0,
          "P.MEDI media type identifier decodes to the RA92 the lab's own "
          "vax.ini attaches (set rq0 ra92)");
    CHECK(bu16(b, 32) == 0, "P.SHUN shadow unit 0 -- nothing shadowed");
    CHECK(bu16(b, 42) == 0, "GUS reserved [42:44] is zero");

    /* Geometry, sec 6.12: blocks/track, tracks/group, groups/cylinder, and the
     * RCT that spans one cylinder (73 * 13 == 949). */
    CHECK(bu16(b, 36) == 73, "P.TRCK track size 73 blocks/track");
    CHECK(bu16(b, 38) == 13, "P.GRP group size 13 tracks/group");
    CHECK(bu16(b, 40) == 1, "P.CYL cylinder size 1 group/cylinder");
    CHECK(bu16(b, 44) == 949 && 73 * 13 == 949,
          "P.RCTS RCT size 949 == track size * group size");
    CHECK(b[46] == 1, "P.RBNS 1 replacement block per track");
    CHECK(b[47] == 1, "P.RCTC 1 RCT copy");

    /* --- THE UNDECODED RESIDUE. Table A-7's last field ends at body[48];
     * this class carries 52 body bytes. Asserted as *present and unexplained*
     * so that a future change which starts MEANING something here has to come
     * back and edit this test -- see spec sec 5. */
    CHECK(bu16(b, 48) == 0x006e,
          "body[48:50] is the constant 0x006e -- NOT DECODED (0x6e == 110 is "
          "this class's own SCA content length, and every frame of the class "
          "has that length, so a length echo and a constant are "
          "indistinguishable here)");
    CHECK(bu16(b, 50) == 0x4231,
          "body[50:52] carries one of the 32 observed values -- NOT DECODED, "
          "and no meaning may be read into it");
    /* The unit-flags word is a documented FIELD with an undocumented VALUE:
     * Table A-5 defines no bit 15. Pinned so the gap cannot be quietly filled. */
    CHECK(bu16(b, 14) == 0x8000,
          "P.UNFL unit flags 0x8000 -- bit 15 is NOT in Table A-5 and is NOT "
          "decoded; it is set identically on the RA92s and the RRD40s, so it "
          "is not a removable-media flag either");
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
    test_gus_end_field_map();
    test_null_guards();

    if (failures == 0) {
        printf("test_scs_mscp: ALL PASSED\n");
        return 0;
    }
    fprintf(stderr, "test_scs_mscp: %d FAILURE(S)\n", failures);
    return 1;
}
