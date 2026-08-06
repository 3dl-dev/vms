/*
 * test_scs_mscp.c - targeted unit tests for the MSCP-over-SCS disk-client
 * command builder/parser (vms-760; field-based since vms-533, Phase B).
 *
 * ORACLE. Every expected byte array below is the byte-exact SCA content of a
 * real frame from af2-firsttimer-established-20260728.pcap (the clean-room
 * observation of a first-timer joining an established VAX1), the ground-truth
 * reference cluster wire. The builders are validated by reproducing those
 * captured joiner MSCP commands byte-for-byte; the parser is validated against
 * the captured VAX1 END responses (SCC-END 0x84 / GUS-END 0x83), proving OVMX
 * reads the echoed correlation token + returned unit/status rather than guessing.
 *
 * WHAT THE BYTE-EXACT TESTS PROVE AFTER vms-533. They used to prove that a
 * captured template plus four substitutions equals the capture -- close to a
 * tautology. The MSCP body is now BUILT from struct scs_mscp_cmd at
 * AA-L619A-TK Table A-6 offsets and not one byte of it is copied from a
 * capture, so the same assertions now prove something real: that the field map
 * transcribed from the public spec reconstructs a real VMS disk class driver's
 * command EXACTLY, and that Phase B changed no transmitted byte.
 */
#include "scs_mscp.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scs_cdt.h" /* vms-8de: struct scs_cdt::send_credit, the live-read oracle */

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
                        uint16_t send_seq, uint32_t cmd_ref, uint16_t unit)
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
    p->incarnation = 1; /* fresh first-timer join -- matches the golden capture */
    p->cmd_ref = cmd_ref;
    p->unit = unit;
}

static void test_scc_byte_exact(void)
{
    struct scs_mscp_params p;
    fill_params(&p, 24, 25,
                SCS_MSCP_CMD_REF(SCS_MSCP_SCC_CLASS, SCS_MSCP_SCC_MSGID0), 0);
    uint8_t out[SCS_MSCP_FRAME_LEN];
    CHECK(scs_mscp_build_scc(&p, out) == 0, "build_scc ok");
    CHECK(memcmp(out + 14, golden_scc, 94) == 0,
          "SCC reproduces golden af2 SCC command byte-exact");
    /* Spot-check the load-bearing MSCP fields. */
    const uint8_t *body = out + 72;
    CHECK(out[30] == SCS_MSCP_MSGTYPE && out[31] == SCS_MSCP_FORMAT,
          "SCC keeps SCS envelope 0x4b/0x13");
    CHECK(body[SCS_MSCP_P_OPCD] == SCS_MSCP_OP_SET_CTLR_CHAR, "SCC opcode 0x04");
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
    fill_params(&p, 26, 27,
                SCS_MSCP_CMD_REF(SCS_MSCP_GUS_CLASS, SCS_MSCP_GUS_MSGID0), 0x0001);
    uint8_t out[SCS_MSCP_FRAME_LEN];
    CHECK(scs_mscp_build_gus(&p, out) == 0, "build_gus ok");
    CHECK(memcmp(out + 14, golden_gus, 94) == 0,
          "GUS reproduces golden af2 GUS command byte-exact");
    const uint8_t *body = out + 72;
    CHECK(body[SCS_MSCP_P_OPCD] == SCS_MSCP_OP_GET_UNIT_STATUS, "GUS opcode 0x03");
    CHECK((uint16_t)(body[10] | (body[11] << 8)) == SCS_MSCP_MOD_NEXT_UNIT,
          "GUS carries NEXT-UNIT modifier 0x0001");
    CHECK((uint16_t)(body[4] | (body[5] << 8)) == 0x0001, "GUS unit word 0x0001");
}

/* The unit/cmd-ref substitution must change ONLY the intended body bytes. */
static void test_gus_unit_substitution(void)
{
    struct scs_mscp_params p;
    uint8_t base[SCS_MSCP_FRAME_LEN], out[SCS_MSCP_FRAME_LEN];
    fill_params(&p, 26, 27,
                SCS_MSCP_CMD_REF(SCS_MSCP_GUS_CLASS, SCS_MSCP_GUS_MSGID0), 0x0001);
    CHECK(scs_mscp_build_gus(&p, base) == 0, "build_gus base ok");

    /* cmd#2 of the enumeration: unit 0x4001, message-id incremented (echoed). */
    p.unit = 0x4001;
    p.cmd_ref = SCS_MSCP_CMD_REF(SCS_MSCP_GUS_CLASS,
                                 (uint16_t)(SCS_MSCP_GUS_MSGID0 + 1));
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
    CHECK(v.status_major == SCS_MSCP_ST_SUCCESS && v.status_subcode == 0,
          "SCC-END status Success, sub-code Normal (sec 6.16: the only status "
          "SET CONTROLLER CHARACTERISTICS ever returns)");
    CHECK(v.end_flags == 0, "SCC-END carries no Table A-3 end flags");
    CHECK(v.modifiers == 0,
          "an END message leaves .modifiers zero -- body[10:12] is P.STS there, "
          "not P.MOD");
    CHECK(v.base_opcode == SCS_MSCP_OP_SET_CTLR_CHAR,
          "SCC-END base opcode strips OP.END back to 0x04");
    /* Correlation token echoed from the joiner's SCC command. */
    CHECK(v.cmd_ref == SCS_MSCP_CMD_REF(SCS_MSCP_SCC_CLASS, SCS_MSCP_SCC_MSGID0),
          "SCC-END echoes the joiner's P.CRF command reference number verbatim");
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
    CHECK(v.status_major == SCS_MSCP_ST_AVAILABLE && v.status_subcode == 0,
          "GUS-END status Unit-Available (4), sub-code 0 -- Table B-2 says "
          "Unit-Available uses no sub-codes");
    /* The returned unit-word drives the NEXT-UNIT enumeration (next = +1). */
    CHECK(v.unit == 0x4000, "GUS-END returns unit-word 0x4000");
    CHECK(v.cmd_ref == SCS_MSCP_CMD_REF(SCS_MSCP_GUS_CLASS, SCS_MSCP_GUS_MSGID0),
          "GUS-END echoes the GUS command reference number");
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

/* ===================== vms-533 -- the FIELD-BASED client =================== */

/*
 * Each MSCP header field lands at its own AA-L619A-TK Table A-6 offset and
 * nowhere else. Built one field at a time off a common base, so a builder that
 * wrote the opcode into body[9], or the modifiers into the status word, or that
 * quietly kept a template byte, cannot pass. This is the test that makes the
 * word "field" mean something -- the byte-exact tests above would still pass
 * against a pure template.
 */
static void test_body_field_positions(void)
{
    struct scs_mscp_cmd c;
    uint8_t base[SCS_MSCP_BODY_LEN], out[SCS_MSCP_BODY_LEN];

    memset(&c, 0, sizeof(c));
    c.cmd_ref = 0x11223344u;
    c.unit = 0x5566;
    c.opcode = SCS_MSCP_OP_GET_UNIT_STATUS;
    c.modifiers = 0x0000;
    CHECK(scs_mscp_build_body(&c, base, sizeof(base)) == 0, "build_body base ok");

    /* P.CRF is a 32-bit little-endian field at body[0:4], not two words the
     * builder happens to write next to each other. */
    CHECK(base[0] == 0x44 && base[1] == 0x33 && base[2] == 0x22 && base[3] == 0x11,
          "P.CRF at body[0:4], little-endian (sec 5.1)");
    CHECK(base[SCS_MSCP_P_UNIT] == 0x66 && base[SCS_MSCP_P_UNIT + 1] == 0x55,
          "P.UNIT at body[4:6]");
    CHECK(base[SCS_MSCP_P_RSVD6] == 0 && base[SCS_MSCP_P_RSVD6 + 1] == 0,
          "the body[6:8] reserved word is zero -- sec 5.2 requires it of a "
          "class driver");
    CHECK(base[SCS_MSCP_P_OPCD] == SCS_MSCP_OP_GET_UNIT_STATUS,
          "P.OPCD at body[8]");
    CHECK(base[SCS_MSCP_P_FLGS] == 0,
          "body[9] is reserved in a COMMAND (P.FLGS is an end-message field)");

    /* --- one field at a time --- */
    c.cmd_ref = 0x11223345u;
    CHECK(scs_mscp_build_body(&c, out, sizeof(out)) == 0, "cmd_ref variant ok");
    CHECK(out[0] == 0x45 && memcmp(out + 4, base + 4, SCS_MSCP_BODY_LEN - 4) == 0,
          "changing cmd_ref moves ONLY body[0:4]");
    c.cmd_ref = 0x11223344u;

    c.unit = 0x5567;
    CHECK(scs_mscp_build_body(&c, out, sizeof(out)) == 0, "unit variant ok");
    CHECK(out[SCS_MSCP_P_UNIT] == 0x67 && memcmp(out, base, SCS_MSCP_P_UNIT) == 0
              && memcmp(out + 6, base + 6, SCS_MSCP_BODY_LEN - 6) == 0,
          "changing unit moves ONLY body[4:6]");
    c.unit = 0x5566;

    c.opcode = SCS_MSCP_OP_ONLINE; /* 0x09, Table A-1, capture-confirmed */
    CHECK(scs_mscp_build_body(&c, out, sizeof(out)) == 0, "opcode variant ok");
    CHECK(out[SCS_MSCP_P_OPCD] == SCS_MSCP_OP_ONLINE
              && memcmp(out, base, SCS_MSCP_P_OPCD) == 0
              && memcmp(out + 9, base + 9, SCS_MSCP_BODY_LEN - 9) == 0,
          "changing opcode moves ONLY body[8] -- not body[9], not the "
          "modifiers word");
    c.opcode = SCS_MSCP_OP_GET_UNIT_STATUS;

    c.modifiers = SCS_MSCP_MOD_NEXT_UNIT;
    CHECK(scs_mscp_build_body(&c, out, sizeof(out)) == 0, "modifiers variant ok");
    CHECK(out[SCS_MSCP_P_MOD] == 0x01 && out[SCS_MSCP_P_MOD + 1] == 0x00
              && memcmp(out, base, SCS_MSCP_P_MOD) == 0
              && memcmp(out + 12, base + 12, SCS_MSCP_BODY_LEN - 12) == 0,
          "changing modifiers moves ONLY body[10:12]");
}

/*
 * The SET CONTROLLER CHARACTERISTICS parameter area, decoded against Table A-6
 * and sec 6.16 -- the region that used to be an opaque "REPLAYED template".
 * These assertions are what let the header claim the values are named rather
 * than copied.
 */
static void test_scc_parameter_area(void)
{
    struct scs_mscp_cmd c;
    uint8_t body[SCS_MSCP_BODY_LEN];
    CHECK(scs_mscp_scc_defaults(&c, SCS_MSCP_CMD_REF(SCS_MSCP_SCC_CLASS,
                                                     SCS_MSCP_SCC_MSGID0)) == 0,
          "scc_defaults ok");
    CHECK(scs_mscp_build_body(&c, body, sizeof(body)) == 0, "scc body ok");

    /* The golden af2 SCC command's parameter area, byte for byte -- the proof
     * that the transcribed field map and the captured bytes are the same thing. */
    CHECK(memcmp(body, golden_scc + SCS_MSCP_BODY_OFF, SCS_MSCP_BODY_LEN) == 0,
          "the field-built SCC body equals the golden af2 SCC body byte for byte");

    CHECK(bu16(body, SCS_MSCP_P_VRSN) == 0,
          "P.VRSN MSCP version 0 -- sec 6.16: the host MUST supply 0 or be "
          "answered Invalid Command");
    CHECK(bu16(body, SCS_MSCP_P_CNTF) == 0x00d0
              && bu16(body, SCS_MSCP_P_CNTF)
                     == (SCS_MSCP_CF_ATTN_MSGS | SCS_MSCP_CF_MISC_ERRLOG
                         | SCS_MSCP_CF_THIS_HOST),
          "P.CNTF 0x00d0 decodes EXACTLY to Table A-4's CF.ATN|CF.MSC|CF.THS -- "
          "the captured byte is a named set of controller flags, not a magic "
          "number");
    CHECK((bu16(body, SCS_MSCP_P_CNTF) & SCS_MSCP_CF_OTHER_HOSTS) == 0,
          "CF.OTH clear: the joiner does not ask for OTHER hosts' error logs");
    CHECK(bu16(body, SCS_MSCP_P_HTMO) == 0,
          "P.HTMO 0 = host-access timeout disabled (sec 6.16)");
    CHECK(bu16(body, 18) == 0, "the Table A-6 reserved word at body[18:20] is 0");
    CHECK(bu32(body, SCS_MSCP_P_TIME) == 0x75280bc0u
              && bu32(body, SCS_MSCP_P_TIME + 4) == 0x00bc0219u,
          "P.TIME is the VMS quadword time of sec 6.16");
    /* AA-L619A-TK sec 6.16: clunks (100 ns) since 00:00 17-Nov-1858. Decoding
     * the captured quadword yields 2026-07-28 12:59:58 UTC -- the wall clock of
     * the af2 capture itself. That is the CALIBRATION of the field (it proves
     * the offset and the epoch), and simultaneously the reason emitting the
     * constant is a labeled replay: a live client sends the CURRENT time. */
    {
        const uint64_t clunks = ((uint64_t)bu32(body, SCS_MSCP_P_TIME + 4) << 32)
                              | bu32(body, SCS_MSCP_P_TIME);
        const uint64_t secs = clunks / 10000000ULL;
        /* days since 17-Nov-1858 to 28-Jul-2026 == 61249; +46798 s == 12:59:58 */
        CHECK(secs / 86400ULL == 61249ULL && secs % 86400ULL == 46798ULL,
              "P.TIME decodes to 1858-11-17 + 61249 days + 12:59:58 == "
              "2026-07-28 12:59:58 UTC, the af2 capture's own wall clock");
    }
    /* sec 5.1 caps the parameter area at 36 bytes; SCC's documented fields end
     * at body[28] and the tail must be zero, not template residue. */
    CHECK(bu32(body, 28) == 0 && bu32(body, 32) == 0,
          "the SCC parameter tail body[28:36] is zero-filled (sec 5.2)");
}

/* GET UNIT STATUS is "standard header only" (sec 6.12): a 24-byte zero tail. */
static void test_gus_parameter_area_is_empty(void)
{
    struct scs_mscp_cmd c;
    uint8_t body[SCS_MSCP_BODY_LEN];
    int i, nonzero = 0;
    CHECK(scs_mscp_gus_defaults(&c, SCS_MSCP_CMD_REF(SCS_MSCP_GUS_CLASS,
                                                     SCS_MSCP_GUS_MSGID0),
                                0x0001) == 0, "gus_defaults ok");
    CHECK(scs_mscp_build_body(&c, body, sizeof(body)) == 0, "gus body ok");
    CHECK(memcmp(body, golden_gus + SCS_MSCP_BODY_OFF, SCS_MSCP_BODY_LEN) == 0,
          "the field-built GUS body equals the golden af2 GUS body byte for byte");
    for (i = SCS_MSCP_HDR_LEN; i < SCS_MSCP_BODY_LEN; i++) {
        if (body[i] != 0) {
            nonzero++;
        }
    }
    CHECK(nonzero == 0,
          "GET UNIT STATUS carries a standard header and nothing else "
          "(sec 6.12) -- body[12:36] is entirely zero");
    CHECK(c.modifiers == SCS_MSCP_MOD_NEXT_UNIT,
          "gus_defaults sets MD.NXU (Table A-2), which is what makes the walk "
          "a walk");
}

/*
 * sec 5.1 makes P.CRF non-zero and OP.END the end-message marker. Both are
 * refused rather than emitted malformed: this client sends COMMANDS, and
 * answering them is Phase D (vms-291), deliberately not here.
 */
static void test_build_body_refusals(void)
{
    struct scs_mscp_cmd c;
    uint8_t body[SCS_MSCP_BODY_LEN];
    CHECK(scs_mscp_gus_defaults(&c, 0x00010001u, 1) == 0, "defaults ok");

    c.cmd_ref = 0;
    CHECK(scs_mscp_build_body(&c, body, sizeof(body)) == -1,
          "a zero command reference number is refused (sec 5.1: unique, "
          "NON-ZERO)");
    c.cmd_ref = 0x00010001u;

    c.opcode = (uint8_t)(SCS_MSCP_OP_GET_UNIT_STATUS | SCS_MSCP_END_BIT);
    CHECK(scs_mscp_build_body(&c, body, sizeof(body)) == -1,
          "an opcode with OP.END set is refused -- that is an END message, and "
          "this module is the disk CLIENT (serving is Phase D / vms-291)");
    c.opcode = SCS_MSCP_OP_GET_UNIT_STATUS;

    CHECK(scs_mscp_build_body(&c, body, SCS_MSCP_BODY_LEN - 1) == -1,
          "a short body buffer is refused");
    CHECK(scs_mscp_build_body(&c, body, sizeof(body)) == 0,
          "and the same command builds once the refusals are removed");
}

/*
 * THE STATUS WORD IS NOT A FLAT CODE (sec 5.6): 5-bit major + 11-bit sub-code.
 *
 * CALIBRATION, not decoration: Table B-2 publishes worked combined values, and
 * the decoder must reproduce them or reading a captured status with it proves
 * nothing. The whole-word comparison this replaces (`status == 3`) is wrong on
 * every one of the Unit-Offline rows below -- which is the live consequence,
 * because Unit-Offline is the GET UNIT STATUS walk's end-of-list terminator.
 */
static void test_status_code_split(void)
{
    /* Table B-2, "Unit-Offline" sub-codes, with the manual's own hex column. */
    CHECK(scs_mscp_status_major(0x0003) == SCS_MSCP_ST_OFFLINE
              && scs_mscp_status_subcode(0x0003) == 0,
          "0x03 = Unit-Offline, sub-code 0 (unit unknown or online elsewhere)");
    CHECK(scs_mscp_status_major(0x0023) == SCS_MSCP_ST_OFFLINE
              && scs_mscp_status_subcode(0x0023) == 1,
          "0x23 = Unit-Offline, sub-code 1 (no volume mounted / RUN-STOP) -- "
          "and 0x23 != 3, which is exactly why the whole word must not be "
          "compared");
    CHECK(scs_mscp_status_major(0x0043) == SCS_MSCP_ST_OFFLINE
              && scs_mscp_status_subcode(0x0043) == 2,
          "0x43 = Unit-Offline, sub-code 2 (unit is inoperative)");
    CHECK(scs_mscp_status_major(0x0083) == SCS_MSCP_ST_OFFLINE
              && scs_mscp_status_subcode(0x0083) == 4,
          "0x83 = Unit-Offline, sub-code 4 (duplicate unit number)");
    CHECK(scs_mscp_status_major(0x0103) == SCS_MSCP_ST_OFFLINE
              && scs_mscp_status_subcode(0x0103) == 8,
          "0x103 = Unit-Offline, sub-code 8 (disabled by field service)");

    /* Table B-2, "Success" sub-codes -- the same arithmetic on a second row. */
    CHECK(scs_mscp_status_major(0x0080) == SCS_MSCP_ST_SUCCESS
              && scs_mscp_status_subcode(0x0080) == 4,
          "0x80 = Success, sub-code 4 (Duplicate Unit Number)");
    CHECK(scs_mscp_status_major(0x0100) == SCS_MSCP_ST_SUCCESS
              && scs_mscp_status_subcode(0x0100) == 8,
          "0x100 = Success, sub-code 8 (Already Online)");
    /* Table B-2, "Write Protected": the two published sub-codes. */
    CHECK(scs_mscp_status_major(0x2006) == SCS_MSCP_ST_WRITE_PROT
              && scs_mscp_status_subcode(0x2006) == 256,
          "0x2006 = Write Protected, sub-code 256 (hardware write protected)");
    CHECK(scs_mscp_status_major(0x1006) == SCS_MSCP_ST_WRITE_PROT
              && scs_mscp_status_subcode(0x1006) == 128,
          "0x1006 = Write Protected, sub-code 128 (software write protected)");

    /* Table B-1 names, and the honest answer for the codes it leaves undefined. */
    CHECK(strcmp(scs_mscp_status_name(SCS_MSCP_ST_SUCCESS), "Success") == 0
              && strcmp(scs_mscp_status_name(SCS_MSCP_ST_OFFLINE),
                        "Unit-Offline") == 0
              && strcmp(scs_mscp_status_name(SCS_MSCP_ST_DRIVE_ERR),
                        "Drive Error") == 0,
          "Table B-1 major-code names");
    CHECK(strcmp(scs_mscp_status_name(12), "undefined status code") == 0,
          "Table B-1 defines nothing at 12 and the decoder does not invent one");
    CHECK(strcmp(scs_mscp_opcode_name(0x83), "GET UNIT STATUS") == 0,
          "an endcode names the command it answers");
    CHECK(strcmp(scs_mscp_opcode_name(0x14),
                 "opcode not confirmed on our wire") == 0,
          "REPLACE (0x14) is in Table A-1 but not on our wire, and is NOT "
          "silently named");
}

/*
 * The live consequence, on a real captured frame: take the golden GUS-END and
 * move its status to a published Unit-Offline sub-code. The walk's terminator
 * test must still fire. Under the old whole-word comparison it did not, and the
 * enumeration would have run past the end of the unit list.
 */
static void test_gus_end_offline_subcode(void)
{
    uint8_t frame[14 + 110];
    struct scs_mscp_view v;
    make_frame(frame, sizeof(frame), golden_gus_end, 110);
    /* body[10:12] = P.STS. 0x0023 = Unit-Offline, sub-code 1 (Table B-2). */
    frame[14 + SCS_MSCP_BODY_OFF + SCS_MSCP_P_STS] = 0x23;
    frame[14 + SCS_MSCP_BODY_OFF + SCS_MSCP_P_STS + 1] = 0x00;
    CHECK(scs_mscp_parse(frame, sizeof(frame), &v) == 0, "parse offline GUS-END");
    CHECK(v.status == 0x0023, "raw status preserved");
    CHECK(v.status_major == SCS_MSCP_ST_OFFLINE,
          "a GUS-END with Unit-Offline sub-code 1 IS the end-of-list "
          "terminator -- v.status == SCS_MSCP_ST_OFFLINE would have missed it");
    CHECK(v.status_subcode == 1, "and the sub-code survives for diagnostics");

    /* Table A-3 end flags ride the same end message and are decoded now. */
    frame[14 + SCS_MSCP_BODY_OFF + SCS_MSCP_P_FLGS] =
        SCS_MSCP_EF_ERROR_LOG_GENERATED;
    CHECK(scs_mscp_parse(frame, sizeof(frame), &v) == 0, "parse flagged GUS-END");
    CHECK(v.end_flags == SCS_MSCP_EF_ERROR_LOG_GENERATED,
          "P.FLGS EF.LOG (Error Log Generated) decoded from body[9]");
}

/* A COMMAND's body[10:12] is P.MOD, not P.STS. The parser must not report a
 * status for one -- reading modifiers as a status is a category error. */
static void test_parse_command_is_not_an_end_message(void)
{
    struct scs_mscp_params p;
    struct scs_mscp_view v;
    uint8_t out[SCS_MSCP_FRAME_LEN];
    fill_params(&p, 26, 27,
                SCS_MSCP_CMD_REF(SCS_MSCP_GUS_CLASS, SCS_MSCP_GUS_MSGID0), 0x0001);
    CHECK(scs_mscp_build_gus(&p, out) == 0, "build_gus ok");
    CHECK(scs_mscp_parse(out, sizeof(out), &v) == 0, "parse our own GUS command");
    CHECK(!v.is_end, "a command is not an end message");
    CHECK(v.opcode == SCS_MSCP_OP_GET_UNIT_STATUS
              && v.base_opcode == SCS_MSCP_OP_GET_UNIT_STATUS,
          "GUS command opcode");
    CHECK(v.modifiers == SCS_MSCP_MOD_NEXT_UNIT,
          "body[10:12] decodes as P.MOD on a command");
    CHECK(v.status == 0 && v.status_major == 0 && v.status_subcode == 0
              && v.end_flags == 0,
          "and NO status is reported for a command -- P.STS does not exist "
          "there (sec 5.1)");
    CHECK(v.cmd_ref == SCS_MSCP_CMD_REF(SCS_MSCP_GUS_CLASS, SCS_MSCP_GUS_MSGID0),
          "P.CRF round-trips build -> parse");
}

static void test_null_guards(void)
{
    uint8_t out[SCS_MSCP_FRAME_LEN];
    struct scs_mscp_params p;
    memset(&p, 0, sizeof(p));
    CHECK(scs_mscp_build_scc(NULL, out) == -1, "build_scc NULL p");
    CHECK(scs_mscp_build_scc(&p, NULL) == -1, "build_scc NULL out");
    CHECK(scs_mscp_build_gus(NULL, out) == -1, "build_gus NULL p");
    /* A zeroed params has cmd_ref 0, which sec 5.1 forbids -- the refusal
     * propagates all the way out of the frame builder. */
    CHECK(scs_mscp_build_scc(&p, out) == -1,
          "build_scc refuses a zero command reference number");
    CHECK(scs_mscp_build_body(NULL, out, SCS_MSCP_BODY_LEN) == -1,
          "build_body NULL cmd");
    CHECK(scs_mscp_scc_defaults(NULL, 1) == -1, "scc_defaults NULL");
    CHECK(scs_mscp_gus_defaults(NULL, 1, 0) == -1, "gus_defaults NULL");
    struct scs_mscp_cmd c;
    CHECK(scs_mscp_gus_defaults(&c, 1, 0) == 0 &&
          scs_mscp_build_command(NULL, &c, out) == -1, "build_command NULL p");
    CHECK(scs_mscp_build_command(&p, NULL, out) == -1, "build_command NULL cmd");
    struct scs_mscp_view v;
    CHECK(scs_mscp_parse(NULL, 108, &v) == -1, "parse NULL frame");
    uint8_t shortf[40] = {0};
    CHECK(scs_mscp_parse(shortf, sizeof(shortf), &v) == -1, "parse short frame");
}

/*
 * vms-8de: the build site itself, not just the wire, must stop replaying the
 * constant 1. With p->cdt pointing at a live CDT, the SCS envelope's credit
 * field ([48:50], abs frame offset 62) must be READ from cdt->send_credit --
 * not from SCS_MSCP_ENV_CREDIT -- and it must track a change to that field.
 * This is the build-site case; the existing byte-exact tests above (p.cdt ==
 * NULL via fill_params' memset) already cover the no-CDT / downstream-
 * overwrite case, which is unchanged by this item.
 */
static void test_credit_reads_live_cdt(void)
{
    struct scs_mscp_params p;
    fill_params(&p, 24, 25,
                SCS_MSCP_CMD_REF(SCS_MSCP_SCC_CLASS, SCS_MSCP_SCC_MSGID0), 0);

    struct scs_cdt cdt;
    memset(&cdt, 0, sizeof(cdt));
    cdt.send_credit = 7; /* deliberately NOT the replayed constant (1) */
    p.cdt = &cdt;

    uint8_t out[SCS_MSCP_FRAME_LEN];
    CHECK(scs_mscp_build_scc(&p, out) == 0, "build_scc with live cdt ok");
    uint16_t credit = (uint16_t)(out[62] | (out[63] << 8));
    CHECK(credit == 7,
          "build site reads credit from cdt->send_credit, not SCS_MSCP_ENV_CREDIT");
    CHECK(credit != SCS_MSCP_ENV_CREDIT,
          "live credit differs from the replayed constant this test chose 7 to prove");

    /* Live tracking: change the account, rebuild, the build site follows it. */
    cdt.send_credit = 3;
    CHECK(scs_mscp_build_gus(&p, out) == 0, "build_gus with live cdt ok");
    credit = (uint16_t)(out[62] | (out[63] << 8));
    CHECK(credit == 3, "build site re-reads cdt->send_credit on each build, not cached");

    /* No CDT (the pre-vms-8de shape) still falls back to the labeled replay. */
    p.cdt = NULL;
    CHECK(scs_mscp_build_scc(&p, out) == 0, "build_scc with no cdt ok");
    credit = (uint16_t)(out[62] | (out[63] << 8));
    CHECK(credit == SCS_MSCP_ENV_CREDIT,
          "no cdt -> unchanged fallback to SCS_MSCP_ENV_CREDIT");
}

int main(void)
{
    test_scc_byte_exact();
    test_gus_byte_exact();
    test_gus_unit_substitution();
    test_parse_scc_end();
    test_parse_gus_end();
    test_gus_end_field_map();
    /* vms-533 -- Phase B, the field-based client. */
    test_body_field_positions();
    test_scc_parameter_area();
    test_gus_parameter_area_is_empty();
    test_build_body_refusals();
    test_status_code_split();
    test_gus_end_offline_subcode();
    test_parse_command_is_not_an_end_message();
    test_null_guards();
    test_credit_reads_live_cdt();

    if (failures == 0) {
        printf("test_scs_mscp: ALL PASSED\n");
        return 0;
    }
    fprintf(stderr, "test_scs_mscp: %d FAILURE(S)\n", failures);
    return 1;
}
