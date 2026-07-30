/*
 * test_scs_dir.c - unit tests for the vms-246 SCS$DIRECTORY connect +
 * SCS$DIR_LOOKUP responder (scs_dir.c).
 *
 * Every asserted byte value is either GROUNDED (spec sec 4h) or a documented
 * REPLAY of a real captured directory frame from
 * formation-ci1-joinwindow.pcap (the golden VAX2-joins-VAX1 handshake):
 *   SCA#21 SCS$DIRECTORY CONNECT-REQUEST, SCA#25 CONNECT-RESPONSE,
 *   SCA#29 MSCP$TAPE lookup REQUEST, SCA#31 MSCP$TAPE "NOT PRESENT HERE",
 *   SCA#37 VMS$VAXcluster lookup REQUEST, SCA#38 VMS$VAXcluster resolved.
 * This is the REQUIRED targeted unit test; it does NOT replace the live-wire
 * SDA proof (item DONE condition), nor is it replaced by it.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scs_dir.h"

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

static void check_bytes(const uint8_t *got, const uint8_t *want, size_t n, const char *what)
{
    check(memcmp(got, want, n) == 0, what);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* OVMX test identity (matches test_scs_connect.c). */
static const uint8_t ovmx_mac[6] = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 };
/* vms-9f3: OVMX's cluster-LOGICAL addr (abs 24), DISTINCT from the raw HW MAC. */
static const uint8_t ovmx_logical[6] = { 0xaa, 0x00, 0x04, 0x00, 0x06, 0x04 };
static const uint8_t vax1_mac[6] = { 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04 };

/* Assemble a full Ethernet+SCA frame: 14-byte header (contents irrelevant to
 * scs_dir_parse, which reads from frame+14) + the captured SCA content. */
static size_t make_frame(uint8_t *out, const uint8_t *sca, size_t sca_len)
{
    memset(out, 0, 14);
    out[12] = 0x60;
    out[13] = 0x07;
    memcpy(out + 14, sca, sca_len);
    return 14 + sca_len;
}

/* --- byte-exact captured SCA contents (formation-ci1-joinwindow.pcap) --- */

/* SCA#21: SCS$DIRECTORY CONNECT-REQUEST (V1->V2), remote 0, local 0x63050008. */
static const uint8_t sca21[110] = {
    0x6c,0x00,0xaa,0x00,0x04,0x00,0x02,0x04,0x01,0x00,0xaa,0x00,0x04,0x00,0x01,0x04,
    0x5b,0x13,0x00,0x00,0x01,0x00,0x01,0x00,0x12,0x00,0x00,0x00,0x00,0x00,0x01,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x02,0x42,0x00,0x04,0x00,0x00,0x00,
    0x03,0x00,0x00,0x00,0x00,0x00,0x08,0x00,0x05,0x63,0x00,0x00,0x01,0x00,0x53,0x43,
    0x53,0x24,0x44,0x49,0x52,0x45,0x43,0x54,0x4f,0x52,0x59,0x20,0x20,0x20,0x53,0x43,
    0x53,0x24,0x44,0x49,0x52,0x5f,0x4c,0x4f,0x4f,0x4b,0x55,0x50,0x20,0x20,0x20,0x20,
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20
};

/* SCA#29: MSCP$TAPE lookup REQUEST (V1->V2), result all-zero. */
static const uint8_t sca29[94] = {
    0x5c,0x00,0xaa,0x00,0x04,0x00,0x02,0x04,0x01,0x00,0xaa,0x00,0x04,0x00,0x01,0x04,
    0x5b,0x13,0x02,0x00,0x03,0x00,0x01,0x00,0x12,0x00,0x02,0x00,0x00,0x00,0x03,0x00,
    0x00,0x00,0x02,0x00,0x00,0x00,0x01,0x00,0x00,0x02,0x32,0x00,0x04,0x00,0x0a,0x00,
    0x00,0x00,0x07,0x00,0x59,0x33,0x08,0x00,0x05,0x63,0x00,0x00,0x00,0x00,0x4d,0x53,
    0x43,0x50,0x24,0x54,0x41,0x50,0x45,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* SCA#37: VMS$VAXcluster lookup REQUEST (V1->V2), opcode 0x4b, result all-zero. */
static const uint8_t sca37[94] = {
    0x5c,0x00,0xaa,0x00,0x04,0x00,0x02,0x04,0x01,0x00,0xaa,0x00,0x04,0x00,0x01,0x04,
    0x4b,0x13,0x05,0x00,0x06,0x00,0x01,0x00,0x12,0x00,0x05,0x00,0x00,0x00,0x06,0x00,
    0x00,0x00,0x05,0x00,0x00,0x00,0x01,0x00,0x00,0x02,0x32,0x00,0x04,0x00,0x0a,0x00,
    0x00,0x00,0x07,0x00,0x59,0x33,0x08,0x00,0x05,0x63,0x00,0x00,0x00,0x00,0x56,0x4d,
    0x53,0x24,0x56,0x41,0x58,0x63,0x6c,0x75,0x73,0x74,0x65,0x72,0x20,0x20,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* SCA#31: MSCP$TAPE lookup RESPONSE (V2->V1), result "NOT PRESENT HERE". */
static const uint8_t sca31[94] = {
    0x5c,0x00,0xaa,0x00,0x04,0x00,0x01,0x04,0x01,0x00,0xaa,0x00,0x04,0x00,0x02,0x04,
    0x5b,0x13,0x03,0x00,0x03,0x00,0x01,0x00,0x12,0x00,0x03,0x00,0x00,0x00,0x03,0x00,
    0x00,0x00,0x03,0x00,0x00,0x00,0x01,0x00,0x00,0x02,0x32,0x00,0x04,0x00,0x0a,0x00,
    0x01,0x00,0x08,0x00,0x05,0x63,0x07,0x00,0x59,0x33,0x01,0x00,0x00,0x00,0x4d,0x53,
    0x43,0x50,0x24,0x54,0x41,0x50,0x45,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x4e,0x4f,
    0x54,0x20,0x50,0x52,0x45,0x53,0x45,0x4e,0x54,0x20,0x48,0x45,0x52,0x45
};

/* LIVE vms-246 lab wire: VAX1->OVMX MSCP$TAPE lookup REQUEST. Unlike the
 * golden SCA#29, its result field [78:94] is NON-zero (request-context bytes);
 * the [58:62] marker is 0 (request). Grounds the request/response discriminator
 * fix -- a result_zero test would misclassify this as a non-request. */
static const uint8_t live_mscptape_req[94] = {
    0x5c,0x00,0xb6,0x16,0x8a,0xdc,0x3a,0x53,0x01,0x00,0xaa,0x00,0x04,0x00,0x01,0x04,
    0x5b,0x13,0x02,0x00,0x03,0x00,0x01,0x00,0x12,0x00,0x02,0x00,0x00,0x00,0x03,0x00,
    0x00,0x00,0x02,0x00,0x00,0x00,0x01,0x00,0x00,0x02,0x32,0x00,0x04,0x00,0x0a,0x00,
    0x00,0x00,0x07,0x00,0x58,0x4f,0x0c,0x00,0x6f,0x35,0x00,0x00,0x00,0x00,0x4d,0x53,
    0x43,0x50,0x24,0x54,0x41,0x50,0x45,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0xfa,0x03,
    0x00,0x24,0xde,0x03,0x00,0x11,0x00,0x00,0x01,0x00,0x01,0x00,0x00,0x00
};

static void test_parse_real(void)
{
    printf("[parse real captured directory frames]\n");
    uint8_t f[128];
    struct scs_dir_view v;

    /* SCS$DIRECTORY CONNECT-REQUEST. */
    size_t n = make_frame(f, sca21, sizeof(sca21));
    check(scs_dir_parse(f, n, &v) == 0, "parse SCA#21 ok");
    check(v.opcode == 0x5b && v.format == 0x13, "SCA#21 opcode 0x5b fmt 0x13 (GROUNDED)");
    check(v.total_sca_len == 110, "SCA#21 total SCA len 110");
    check(v.remote_conid == 0x00000000u, "SCA#21 remote Con.ID 0 (peer's not yet known, GROUNDED)");
    check(v.local_conid == 0x63050008u, "SCA#21 local Con.ID 0x63050008 (VAX1 dir handle, GROUNDED)");
    check(memcmp(v.name, "SCS$DIRECTORY   ", 16) == 0, "SCA#21 queried name 'SCS$DIRECTORY' (GROUNDED)");
    check(v.is_dir_connect_request == 1, "SCA#21 classified as SCS$DIRECTORY CONNECT-REQUEST");
    check(v.is_lookup_request == 0, "SCA#21 not a lookup request");

    /* MSCP$TAPE lookup REQUEST (result all-zero => request). */
    n = make_frame(f, sca29, sizeof(sca29));
    check(scs_dir_parse(f, n, &v) == 0, "parse SCA#29 ok");
    check(v.op == 0x000a, "SCA#29 directory-operation [46:48] == 0x0a (lookup, inferred)");
    check(memcmp(v.name, "MSCP$TAPE       ", 16) == 0, "SCA#29 queried name 'MSCP$TAPE' (GROUNDED)");
    check(v.result_zero == 1, "SCA#29 result field all-zero => it is a REQUEST (GROUNDED)");
    check(v.is_lookup_request == 1, "SCA#29 classified as lookup REQUEST");

    /* VMS$VAXcluster lookup REQUEST (opcode 0x4b once the connection is up). */
    n = make_frame(f, sca37, sizeof(sca37));
    check(scs_dir_parse(f, n, &v) == 0, "parse SCA#37 ok");
    check(v.opcode == 0x4b, "SCA#37 opcode 0x4b (lookups switch to 0x4b, GROUNDED)");
    check(memcmp(v.name, "VMS$VAXcluster  ", 16) == 0, "SCA#37 queried name 'VMS$VAXcluster' (GROUNDED)");
    check(v.is_lookup_request == 1, "SCA#37 classified as lookup REQUEST");

    /* MSCP$TAPE lookup RESPONSE (result NOT zero => not a request). */
    n = make_frame(f, sca31, sizeof(sca31));
    check(scs_dir_parse(f, n, &v) == 0, "parse SCA#31 ok");
    check(v.result_zero == 0, "SCA#31 result field non-zero => it is a RESPONSE");
    check(v.is_lookup_request == 0, "SCA#31 (a response) NOT classified as a request");

    /* LIVE VAX lookup REQUEST with a NON-zero result field: must still classify
     * as a request via the [58:62] marker (the vms-246 stall root-cause fix). */
    n = make_frame(f, live_mscptape_req, sizeof(live_mscptape_req));
    check(scs_dir_parse(f, n, &v) == 0, "parse live MSCP$TAPE request ok");
    check(v.op == 0x000a, "live request op == 0x0a");
    check(v.result_zero == 0, "live request result field is NON-zero (unlike golden SCA#29)");
    check(v.marker == 0, "live request [58:62] marker == 0 (request)");
    check(v.is_lookup_request == 1,
          "live request classified as lookup REQUEST via marker (NOT result_zero)");
    check(v.remote_conid == 0x4f580007u, "live request addresses OVMX dir handle 0x4F580007");

    check(scs_dir_parse(NULL, n, &v) == -1, "NULL frame rejected");
    check(scs_dir_parse(f, 40, &v) == -1, "too-short frame rejected");
}

static void test_build_connect_response(void)
{
    printf("[build SCS$DIRECTORY CONNECT-RESPONSE]\n");
    struct scs_dir_params p;
    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, vax1_mac, 6);
    memcpy(p.src_mac, ovmx_mac, 6);
    memcpy(p.src_logical, ovmx_logical, 6); /* vms-9f3: abs-24 cluster-logical addr */
    memcpy(p.peer_logical, vax1_mac, 6);
    p.remote_conid = 0x63050008u;      /* the VAX's SCS$DIRECTORY handle (learned) */
    p.local_conid = SCS_DIR_OVMX_CONID; /* OVMX's own handle (design choice) */
    p.recv_ack = 3;
    p.send_seq = 4;

    uint8_t out[SCS_DIR_RESP_FRAME_LEN];
    memset(out, 0xAA, sizeof(out));
    check(scs_dir_build_connect_response(&p, out) == 0, "build_connect_response succeeds");

    check_bytes(out + 0, vax1_mac, 6, "Ethernet dst == peer MAC");
    check_bytes(out + 6, ovmx_mac, 6, "Ethernet src == OVMX HW MAC");
    check(out[12] == 0x60 && out[13] == 0x07, "ethertype 0x6007");
    check(out[14] == 0x6c && out[15] == 0x00, "SCA length 0x006c (total 110)");
    check_bytes(out + 16, vax1_mac, 6, "SCA dest logical == peer_logical (abs 16)");
    check_bytes(out + 24, ovmx_logical, 6, "SCA src-logical == cluster-LOGICAL addr, NOT HW MAC (abs 24, vms-9f3)");
    check(memcmp(out + 24, ovmx_mac, 6) != 0, "src-logical (abs 24) DISTINCT from raw HW MAC (vms-9f3)");
    check(out[30] == 0x5b && out[31] == 0x13, "opcode 0x5b, format 0x13 (abs 30/31, GROUNDED)");

    /* THE ADMISSION ACT: Con.ID pair bound (spec sec 4h/4g). */
    check(le32(out + 64) == 0x63050008u, "Remote Con.ID == peer's dir handle (abs 64, echoed)");
    check(le32(out + 68) == (uint32_t)SCS_DIR_OVMX_CONID, "Local Con.ID == OVMX's dir handle (abs 68)");
    check(le32(out + 68) != 0, "Local Con.ID is non-zero (own handle supplied -> pair bound)");

    /* Sequence counters (spec sec 4h(4)): send_seq mirrored at [20:22]/[30:32]. */
    check(le16(out + 14 + 18) == 3, "recv_ack [18:20] == 3");
    check(le16(out + 14 + 20) == 4, "send_seq [20:22] == 4");
    check(le16(out + 14 + 30) == 4, "send_seq mirror [30:32] == 4 (GROUNDED == [20:22])");
    check(le16(out + 14 + 26) == 3, "recv_ack mirror [26:28] == 3");

    /* Grounded template constants + inner length. */
    check(le16(out + 14 + 24) == 18, "[24:26] == 18 (NISCS_LAN_OVRHD, GROUNDED)");
    check(le16(out + 14 + 42) == 66, "inner length [42:44] == payload-44 == 66 (GROUNDED)");
    check(le16(out + 14 + 46) == 2, "directory-operation [46:48] == 2 (CONNECT-RESPONSE, replayed)");

    /* SYSAP name / result fields (replayed byte-exact from SCA#25). */
    check_bytes(out + 14 + 62, (const uint8_t *)"SCS$DIR_LOOKUP  ", 16, "name [62:78] == 'SCS$DIR_LOOKUP'");
    check_bytes(out + 14 + 78, (const uint8_t *)"SCS$DIRECTORY   ", 16, "result [78:94] == 'SCS$DIRECTORY'");

    /* Round-trip: parse our own response back. */
    struct scs_dir_view v;
    check(scs_dir_parse(out, sizeof(out), &v) == 0, "parse our CONNECT-RESPONSE ok");
    check(v.remote_conid == 0x63050008u && v.local_conid == (uint32_t)SCS_DIR_OVMX_CONID,
          "round-trip Con.ID pair matches");

    /* CONNECT-ECHO (op=1): remote echoed, local still 0. */
    uint8_t eout[SCS_DIR_ECHO_FRAME_LEN];
    check(scs_dir_build_connect_echo(&p, eout) == 0, "build_connect_echo succeeds");
    check(eout[30] == 0x5b, "echo opcode 0x5b");
    check(le32(eout + 64) == 0x63050008u, "echo Remote Con.ID == peer handle (abs 64)");
    check(le32(eout + 68) == 0u, "echo Local Con.ID == 0 (own not yet assigned, GROUNDED)");
    check(le16(eout + 14 + 46) == 1, "echo directory-operation [46:48] == 1 (replayed)");

    check(scs_dir_build_connect_response(NULL, out) == -1, "NULL params rejected");
    check(scs_dir_build_connect_echo(NULL, eout) == -1, "NULL params rejected (echo)");
}

static void test_build_lookup_response(void)
{
    printf("[build SCS$DIR_LOOKUP response]\n");

    /* Affirmative: OVMX serves VMS$VAXcluster (the connection manager). */
    struct scs_dir_lookup_params lp;
    memset(&lp, 0, sizeof(lp));
    memcpy(lp.dst_mac, vax1_mac, 6);
    memcpy(lp.src_mac, ovmx_mac, 6);
    memcpy(lp.src_logical, ovmx_logical, 6); /* vms-9f3: abs-24 cluster-logical addr */
    memcpy(lp.peer_logical, vax1_mac, 6);
    lp.remote_conid = 0x63050008u;
    lp.local_conid = SCS_DIR_OVMX_CONID;
    lp.recv_ack = 6;
    lp.send_seq = 6;
    lp.opcode = 0x4b;     /* echo the request opcode */
    lp.op = 0x0a;
    memcpy(lp.name, "VMS$VAXcluster", 14);
    lp.affirmative = 1;

    uint8_t out[SCS_DIR_LOOKUP_FRAME_LEN];
    memset(out, 0xAA, sizeof(out));
    check(scs_dir_build_lookup_response(&lp, out) == 0, "build_lookup_response (affirmative) succeeds");
    check(out[30] == 0x4b && out[31] == 0x13, "opcode echoed 0x4b, format 0x13");
    check(le32(out + 64) == 0x63050008u, "Remote Con.ID == peer dir handle (abs 64)");
    check(le32(out + 68) == (uint32_t)SCS_DIR_OVMX_CONID, "Local Con.ID == OVMX dir handle (abs 68)");
    check(le16(out + 14 + 46) == 0x0a, "directory-operation [46:48] == 0x0a (echoed)");
    check(out[14 + 58] == 0x01 && out[14 + 59] == 0x00, "[58:62] response marker 0x00000001");
    check_bytes(out + 14 + 62, (const uint8_t *)"VMS$VAXcluster  ", 16, "queried name echoed into [62:78]");
    /* The AFFIRMATIVE result descriptor, reproduced byte-exact from SCA#38
     * (ungrounded semantics, spec sec 4h RE gap (c)). */
    static const uint8_t affirm[16] = {
        0x01,0x1b,0x01,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x06,0x00
    };
    check_bytes(out + 14 + 78, affirm, 16, "result [78:94] == VMS$VAXcluster descriptor (SCA#38 replay)");
    check(le16(out + 14 + 20) == 6, "send_seq [20:22] == 6");
    check(le16(out + 14 + 30) == 6, "send_seq mirror [30:32] == 6");

    /* Negative: OVMX does not serve MSCP$TAPE -> "NOT PRESENT HERE". */
    struct scs_dir_lookup_params np = lp;
    memset(np.name, 0, sizeof(np.name));
    memcpy(np.name, "MSCP$TAPE", 9);
    np.affirmative = 0;
    np.opcode = 0x5b;
    check(scs_dir_build_lookup_response(&np, out) == 0, "build_lookup_response (negative) succeeds");
    check(out[30] == 0x5b, "negative opcode echoed 0x5b");
    check_bytes(out + 14 + 62, (const uint8_t *)"MSCP$TAPE       ", 16, "name echoed 'MSCP$TAPE'");
    check_bytes(out + 14 + 78, (const uint8_t *)"NOT PRESENT HERE", 16,
                "result [78:94] == 'NOT PRESENT HERE' (GROUNDED negative marker, spec 4h(2))");

    check(scs_dir_build_lookup_response(NULL, out) == -1, "NULL params rejected");
}

static void test_incarnation_echo(void)
{
    printf("[established-join node-incarnation echo into [22:24] (spec sec 4i)]\n");
    struct scs_dir_params p;
    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, vax1_mac, 6);
    memcpy(p.src_mac, ovmx_mac, 6);
    memcpy(p.src_logical, ovmx_logical, 6); /* vms-9f3: abs-24 cluster-logical addr */
    memcpy(p.peer_logical, vax1_mac, 6);
    p.remote_conid = 0x36f2000cu;
    p.local_conid = SCS_DIR_OVMX_CONID;
    p.recv_ack = 1;
    p.send_seq = 2;

    uint8_t out[SCS_DIR_RESP_FRAME_LEN];

    /* incarnation==0 leaves the fresh-golden template value 1 (byte-exact
     * reproduction of the fresh-formation joiner is preserved). */
    p.incarnation = 0;
    check(scs_dir_build_connect_response(&p, out) == 0, "build response (incarnation=0)");
    check(le16(out + 14 + 22) == 1, "[22:24] == 1 when incarnation=0 (fresh golden preserved)");

    /* incarnation==3 (the value observed on the live established-join wire,
     * VAX1's own connect-request [22:24]) is echoed into [22:24] -- and ONLY
     * [22:24]; the send_seq mirror [30:32] is unaffected. */
    p.incarnation = 3;
    check(scs_dir_build_connect_response(&p, out) == 0, "build response (incarnation=3)");
    check(le16(out + 14 + 22) == 3, "[22:24] == 3 echoed (established-join, observed off the wire)");
    check(le16(out + 14 + 30) == 2, "send_seq mirror [30:32] still == send_seq (2), not the incarnation");
    check(le16(out + 14 + 20) == 2, "send_seq [20:22] unaffected by incarnation echo");

    /* Same for the CONNECT-ECHO. */
    uint8_t eout[SCS_DIR_ECHO_FRAME_LEN];
    check(scs_dir_build_connect_echo(&p, eout) == 0, "build echo (incarnation=3)");
    check(le16(eout + 14 + 22) == 3, "echo [22:24] == 3 echoed");

    /* And the lookup response. */
    struct scs_dir_lookup_params lp;
    memset(&lp, 0, sizeof(lp));
    memcpy(lp.dst_mac, vax1_mac, 6);
    memcpy(lp.src_mac, ovmx_mac, 6);
    memcpy(lp.src_logical, ovmx_logical, 6); /* vms-9f3: abs-24 cluster-logical addr */
    memcpy(lp.peer_logical, vax1_mac, 6);
    lp.remote_conid = 0x36f2000cu;
    lp.local_conid = SCS_DIR_OVMX_CONID;
    lp.recv_ack = 6; lp.send_seq = 6; lp.opcode = 0x4b; lp.op = 0x0a;
    memcpy(lp.name, "VMS$VAXcluster", 14);
    lp.affirmative = 1;
    lp.incarnation = 3;
    uint8_t lout[SCS_DIR_LOOKUP_FRAME_LEN];
    check(scs_dir_build_lookup_response(&lp, lout) == 0, "build lookup response (incarnation=3)");
    check(le16(lout + 14 + 22) == 3, "lookup response [22:24] == 3 echoed");
}

static void test_build_connect_confirm(void)
{
    printf("[build dir op=3 CONNECT-CONFIRM (vms-760)]\n");
    struct scs_dir_params p;
    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, vax1_mac, 6);
    memcpy(p.src_mac, ovmx_mac, 6);
    memcpy(p.src_logical, ovmx_logical, 6);
    memcpy(p.peer_logical, vax1_mac, 6);
    p.remote_conid = 0xe2dc0008u;              /* member's dir handle */
    p.local_conid = SCS_DIR_OVMX_JOINER_CONID; /* OVMX's own dir-client handle */
    p.recv_ack = 2;
    p.send_seq = 2;
    p.incarnation = 0;

    uint8_t out[SCS_DIR_CONFIRM_FRAME_LEN];
    memset(out, 0xAA, sizeof(out));
    check(scs_dir_build_connect_confirm(&p, out) == 0, "build_connect_confirm succeeds");

    check(out[12] == 0x60 && out[13] == 0x07, "ethertype 0x6007");
    check(out[14 + 0] == 0x3c && out[14 + 1] == 0x00, "SCA length word 0x003c (total 62)");
    check(out[30] == 0x5b && out[31] == 0x13, "msgtype 0x5b, format 0x13 (abs 30/31)");
    check(le16(out + 14 + 46) == SCS_DIR_OP_CONFIRM, "op [46:48] == 3 (connect-confirm)");
    check(le32(out + 14 + 50) == 0xe2dc0008u, "remote Con.ID == member dir handle [50:54]");
    check(le32(out + 14 + 54) == SCS_DIR_OVMX_JOINER_CONID, "local Con.ID == OVMX dir-client handle [54:58]");
    /* marker 0x00010000 at [58:62] (baked in, not substituted). */
    check(le16(out + 14 + 58) == 0x0000 && le16(out + 14 + 60) == 0x0001,
          "marker [58:62] == 0x00010000");
    /* live counters threaded. */
    check(le16(out + 14 + 18) == 2 && le16(out + 14 + 20) == 2,
          "recv_ack/send_seq threaded [18:20]/[20:22]");
    check(le16(out + 14 + 30) == 2, "send_seq mirror [30:32] == 2");
    /* No SYSAP names: the 62-byte SCA frame ends at the marker (abs 76). */
    check(sizeof(out) == 76, "frame length 76 (62-byte SCA, no names)");

    /* Con.ID substitution independent of the template. */
    p.remote_conid = 0x12340008u;
    p.local_conid = 0x4F580008u;
    p.incarnation = 3;
    check(scs_dir_build_connect_confirm(&p, out) == 0, "build_connect_confirm (2)");
    check(le32(out + 14 + 50) == 0x12340008u, "remote Con.ID threaded");
    check(le32(out + 14 + 54) == 0x4F580008u, "local Con.ID threaded");
    check(le16(out + 14 + 22) == 3, "incarnation 3 echoed into [22:24]");

    check(scs_dir_build_connect_confirm(NULL, out) == -1, "NULL params rejected");
    check(scs_dir_build_connect_confirm(&p, NULL) == -1, "NULL out rejected");
}

int main(void)
{
    printf("test_scs_dir: SCS$DIRECTORY connect + SCS$DIR_LOOKUP (vms-246)\n");
    test_parse_real();
    test_build_connect_response();
    test_build_lookup_response();
    test_incarnation_echo();
    test_build_connect_confirm();
    printf("test_scs_dir: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
