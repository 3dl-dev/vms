/*
 * test_scs_connect.c - unit tests for the vms-5fe directed-HELLO builder
 * and the VMS$VAXcluster SCS connect builder/parser.
 *
 * Every asserted byte value is either GROUNDED (spec sec 4b/4d/4g) or a
 * documented REPLAY of a real captured connect frame
 * (formation-ci1-joinwindow.pcap raw frames 47/50). This is the REQUIRED
 * targeted unit test; it does NOT replace the live-wire SDA proof (item
 * DONE condition), nor is it replaced by it.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scs_conn.h" /* vms-dd5: the connection state machine + wire->event map */
#include "scs_connect.h"
#include "scs_hello.h"

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

/* OVMX test identity. */
static const uint8_t ovmx_mac[6] = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 }; /* "OVX", local bit */
/* vms-9f3: OVMX's cluster-LOGICAL addr (abs 24), DISTINCT from the raw HW MAC. */
static const uint8_t ovmx_logical[6] = { 0xaa, 0x00, 0x04, 0x00, 0x06, 0x04 };
/* VAX1 (DECnet node): Ethernet src == its logical LAVC addr. */
static const uint8_t vax1_mac[6] = { 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04 };

static void test_directed_hello(void)
{
    printf("[directed HELLO]\n");
    struct scs_hello_params p;
    memset(&p, 0, sizeof(p));
    /* dst_mac deliberately set to the multicast group to prove the directed
     * builder IGNORES it in favor of peer_mac. */
    scs_hello_multicast_addr(SCS_HELLO_MCAST_GROUP1, p.dst_mac);
    memcpy(p.src_mac, ovmx_mac, 6);
    memcpy(p.src_logical, ovmx_logical, 6);
    strncpy(p.node_name, "OVMX", sizeof(p.node_name) - 1);
    p.timer_tick = 0;

    static const uint8_t nonce[4] = SCS_HELLO_LAB_NONCE_BYTES;
    uint8_t out[SCS_HELLO_FRAME_LEN];
    memset(out, 0xAA, sizeof(out));
    int rc = scs_hello_build_directed_frame(&p, vax1_mac, nonce, 1,
                                            SCS_HELLO_PFW_REQUEST, out);
    check(rc == 0, "scs_hello_build_directed_frame succeeds");

    /* Directed frame is addressed to the PEER (abs 0), and abs 16 mirrors it. */
    check_bytes(out + 0, vax1_mac, 6, "Ethernet dst == peer MAC (directed, not multicast)");
    check_bytes(out + 16, vax1_mac, 6, "SCA dest logical addr mirrors peer MAC (abs 16)");
    check_bytes(out + 6, ovmx_mac, 6, "Ethernet src == OVMX HW MAC");
    check_bytes(out + 24, ovmx_logical, 6, "SCA src-logical == cluster-LOGICAL addr, NOT HW MAC (abs 24, vms-9f3)");
    check(memcmp(out + 24, ovmx_mac, 6) != 0, "src-logical (abs 24) DISTINCT from raw HW MAC (vms-9f3)");
    check_bytes(out + 120, ovmx_mac, 6, "HELLO-tail HW MAC == raw HW MAC (abs 120, unchanged)");

    /* GROUNDED directed markers (spec sec 4b). */
    static const uint8_t nonce_want[4] = SCS_HELLO_LAB_NONCE_BYTES;
    check_bytes(out + 68, nonce_want, 4, "join nonce == ee-05-39-5b (GROUNDED present, REPLAYED value)");
    check(out[92] == 0x01 && out[93] == 0x00,
          "directed-HELLO flag / incarnation == 0x0001 for a fresh contact (GROUNDED, spec 4b/4i.B)");
    check(out[128] == 0x1f && out[129] == 0x00, "poller-sweep marker == 0x001f=31 (GROUNDED)");
    /* abs-30 channel-verify word: b3 REQUEST here (GROUNDED, spec sec 4a offset-30). */
    check(out[30] == SCS_HELLO_PFW_REQUEST && out[31] == 0x00,
          "per-frame word == 0xb300 (b3 REQUEST, GROUNDED sec 4a)");

    /* Length + node name unchanged from the multicast template. */
    check(out[14] == 0x76 && out[15] == 0x00, "SCA length field == 0x0076 (total 120)");
    check(out[40] == 6, "node-name length prefix == 6");
    check_bytes(out + 41, (const uint8_t *)"OVMX  ", 6, "node name == 'OVMX  '");

    check(scs_hello_build_directed_frame(NULL, vax1_mac, nonce, 1, SCS_HELLO_PFW_REQUEST, out) == -1, "NULL params rejected");
    check(scs_hello_build_directed_frame(&p, NULL, nonce, 1, SCS_HELLO_PFW_REQUEST, out) == -1, "NULL peer rejected");
    check(scs_hello_build_directed_frame(&p, vax1_mac, NULL, 1, SCS_HELLO_PFW_REQUEST, out) == -1, "NULL nonce rejected");
}

/* Byte-exact 124-byte real CONNECT-REQUEST (raw frame 47) and
 * CONNECT-RESPONSE (raw frame 50), for grounding the parser. */
static const uint8_t real_request[124] = {
    0x08,0x00,0x2b,0x78,0x56,0xb9, 0xaa,0x00,0x04,0x00,0x01,0x04, 0x60,0x07, 0x6c,0x00,
    0xaa,0x00,0x04,0x00,0x02,0x04, 0x01,0x00, 0xaa,0x00,0x04,0x00,0x01,0x04, 0x4b,0x13,
    0x06,0x00,0x07,0x00,0x01,0x00,0x12,0x00, 0x06,0x00,0x00,0x00,0x07,0x00,0x00,0x00,
    0x06,0x00,0x00,0x00,0x01,0x00,0x00,0x02, 0x42,0x00,0x04,0x00,0x00,0x00,0x0a,0x00,
    0x00,0x00,0x00,0x00, 0x09,0x00,0xc5,0x62, 0x00,0x00,0x01,0x00, 0x56,0x4d,0x53,0x24,
    0x56,0x41,0x58,0x63,0x6c,0x75,0x73,0x74,0x65,0x72,0x20,0x20, 0x56,0x4d,0x53,0x24,
    0x56,0x41,0x58,0x63,0x6c,0x75,0x73,0x74,0x65,0x72,0x20,0x20, 0x01,0x1b,0x01,0x03,
    0x01,0x00,0x01,0x00,0x01,0x00,0x01,0x08,0x00,0x00,0x06,0x00
};
static const uint8_t real_response[124] = {
    0xaa,0x00,0x04,0x00,0x01,0x04, 0x08,0x00,0x2b,0x78,0x56,0xb9, 0x60,0x07, 0x6c,0x00,
    0xaa,0x00,0x04,0x00,0x01,0x04, 0x01,0x00, 0xaa,0x00,0x04,0x00,0x02,0x04, 0x4b,0x13,
    0x07,0x00,0x08,0x00,0x01,0x00,0x12,0x00, 0x07,0x00,0x00,0x00,0x08,0x00,0x00,0x00,
    0x07,0x00,0x00,0x00,0x01,0x00,0x00,0x02, 0x42,0x00,0x04,0x00,0x02,0x00,0x0a,0x00,
    0x09,0x00,0xc5,0x62, 0x08,0x00,0x58,0x33, 0x00,0x00,0x00,0x00, 0x56,0x4d,0x53,0x24,
    0x56,0x41,0x58,0x63,0x6c,0x75,0x73,0x74,0x65,0x72,0x20,0x20, 0x56,0x4d,0x53,0x24,
    0x56,0x41,0x58,0x63,0x6c,0x75,0x73,0x74,0x65,0x72,0x20,0x20, 0x01,0x1b,0x01,0x03,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x06,0x00
};

static void test_parse_real_frames(void)
{
    printf("[parse real captured connect frames]\n");
    struct scs_connect_view v;

    check(scs_connect_parse(real_request, sizeof(real_request), &v) == 0, "parse CONNECT-REQUEST ok");
    check(v.total_sca_len == 110, "REQUEST total SCA len == 110");
    check(v.msgtype == SCS_MSGTYPE_SEQAPP, "REQUEST msgtype == 0x4b (GROUNDED)");
    check(v.format == SCS_FORMAT_CONST, "REQUEST format == 0x13 (GROUNDED)");
    check(v.has_conid == 1, "REQUEST is a Con.ID-bearing class");
    check(v.remote_conid == 0x00000000u, "REQUEST remote Con.ID == 0 (GROUNDED)");
    check(v.local_conid == 0x62C50009u, "REQUEST local Con.ID == 0x62C50009 (VAX1, GROUNDED)");

    check(scs_connect_parse(real_response, sizeof(real_response), &v) == 0, "parse CONNECT-RESPONSE ok");
    check(v.remote_conid == 0x62C50009u, "RESPONSE remote Con.ID == 0x62C50009 (echoed, GROUNDED)");
    check(v.local_conid == 0x33580008u, "RESPONSE local Con.ID == 0x33580008 (VAX2, GROUNDED)");

    check(scs_connect_parse(NULL, 124, &v) == -1, "NULL frame rejected");
    check(scs_connect_parse(real_request, 20, &v) == -1, "too-short frame rejected");
}

static void test_build_request(void)
{
    printf("[build CONNECT-REQUEST]\n");
    struct scs_connect_params cp;
    memset(&cp, 0, sizeof(cp));
    memcpy(cp.dst_mac, vax1_mac, 6);
    memcpy(cp.src_mac, ovmx_mac, 6);
    memcpy(cp.src_logical, ovmx_logical, 6);
    memcpy(cp.peer_logical, vax1_mac, 6);
    cp.local_conid = SCS_CONNECT_OVMX_CONID_BASE | 0x0001u;
    cp.remote_conid = 0; /* ignored */

    uint8_t out[SCS_CONNECT_FRAME_LEN];
    memset(out, 0xAA, sizeof(out));
    check(scs_connect_build_request(&cp, out) == 0, "build_request succeeds");

    check_bytes(out + 0, vax1_mac, 6, "Ethernet dst == peer MAC");
    check_bytes(out + 6, ovmx_mac, 6, "Ethernet src == OVMX HW MAC");
    check(out[12] == 0x60 && out[13] == 0x07, "ethertype 0x6007");
    check(out[14] == 0x6c && out[15] == 0x00, "SCA length field 0x006c (total 110)");
    check_bytes(out + 16, vax1_mac, 6, "SCA dest logical == peer_logical (abs 16)");
    check_bytes(out + 24, ovmx_logical, 6, "SCA src-logical == cluster-LOGICAL addr, NOT HW MAC (abs 24, vms-9f3)");
    check(memcmp(out + 24, ovmx_mac, 6) != 0, "src-logical (abs 24) DISTINCT from raw HW MAC (vms-9f3)");
    check(out[30] == 0x4b && out[31] == 0x13, "msgtype 0x4b, format 0x13 (abs 30/31, GROUNDED)");
    check(le32(out + 64) == 0x00000000u, "Remote Con.ID == 0 (CONNECT-REQUEST, abs 64)");
    check(le32(out + 68) == (SCS_CONNECT_OVMX_CONID_BASE | 0x0001u), "Local Con.ID == OVMX's (abs 68)");
    /* SYSAP names preserved from the template (abs 76 / abs 92). */
    check_bytes(out + 76, (const uint8_t *)"VMS$VAXcluster  ", 16, "local SYSAP name (abs 76, GROUNDED)");
    check_bytes(out + 92, (const uint8_t *)"VMS$VAXcluster  ", 16, "remote SYSAP name (abs 92, GROUNDED)");

    /* Round-trip through the parser. */
    struct scs_connect_view v;
    check(scs_connect_parse(out, sizeof(out), &v) == 0, "parse our REQUEST ok");
    check(v.remote_conid == 0 && v.local_conid == (SCS_CONNECT_OVMX_CONID_BASE | 0x0001u),
          "round-trip Con.ID pair matches");
}

static void test_build_response(void)
{
    printf("[build CONNECT-RESPONSE]\n");
    struct scs_connect_params cp;
    memset(&cp, 0, sizeof(cp));
    memcpy(cp.dst_mac, vax1_mac, 6);
    memcpy(cp.src_mac, ovmx_mac, 6);
    memcpy(cp.src_logical, ovmx_logical, 6);
    memcpy(cp.peer_logical, vax1_mac, 6);
    cp.local_conid = SCS_CONNECT_OVMX_CONID_BASE | 0x0001u;
    cp.remote_conid = 0x62C50009u; /* echo the peer's (VAX1) Con.ID */

    uint8_t out[SCS_CONNECT_FRAME_LEN];
    check(scs_connect_build_response(&cp, out) == 0, "build_response succeeds");
    check(out[30] == 0x4b && out[31] == 0x13, "msgtype 0x4b, format 0x13");
    check(le32(out + 64) == 0x62C50009u, "Remote Con.ID == echoed peer Con.ID (abs 64)");
    check(le32(out + 68) == (SCS_CONNECT_OVMX_CONID_BASE | 0x0001u), "Local Con.ID == OVMX's (abs 68)");
    check(le32(out + 64) != 0, "RESPONSE remote Con.ID is non-zero (admission act)");

    check(scs_connect_build_request(NULL, out) == -1, "build_request NULL rejected");
    check(scs_connect_build_response(NULL, out) == -1, "build_response NULL rejected");
}

/*
 * vms-c6d: the CONNECT-RESPONSE must carry OVMX's LIVE SCS VC counters, NOT the
 * golden template's replayed 7/8. This is the completing fix: with the golden
 * counters baked in, the VAX rejected OVMX's accept (its live VC had advanced
 * past the directory phase) and retransmitted its 0x4b forever. Assert the live
 * recv_ack / send_seq land at their GROUNDED offsets (spec sec 4h(4)): recv_ack
 * at [18:20]/[26:28]/[34:36] (abs 32/40/48), send_seq at [20:22] mirrored
 * [30:32] (abs 34/44), and the incarnation echo at [22:24] (abs 36).
 */
static void test_response_live_counters(void)
{
    printf("[CONNECT-RESPONSE threads LIVE VC counters (vms-c6d)]\n");
    struct scs_connect_params cp;
    memset(&cp, 0, sizeof(cp));
    memcpy(cp.dst_mac, vax1_mac, 6);
    memcpy(cp.src_mac, ovmx_mac, 6);
    memcpy(cp.src_logical, ovmx_logical, 6);
    memcpy(cp.peer_logical, vax1_mac, 6);
    cp.local_conid = SCS_CONNECT_OVMX_CONID_BASE | 0x0001u;
    cp.remote_conid = 0x62C50009u;          /* echo the VAX's Con.ID */
    /* Live counters deliberately DIFFERENT from the golden 7/8 (proves they are
     * threaded, not replayed): OVMX's VC advanced to send_seq=19 through the
     * directory phase, and the VAX's last send_seq (our recv_seq) was 17. */
    cp.recv_ack = 17;
    cp.send_seq = 19;
    cp.incarnation = 3;                     /* established-join: member advertised N=3 */

    uint8_t out[SCS_CONNECT_FRAME_LEN];
    memset(out, 0xAA, sizeof(out));
    check(scs_connect_build_response(&cp, out) == 0, "build_response (live counters) succeeds");

    /* Con.ID binding (the admission act) still correct. */
    check(le32(out + 64) == 0x62C50009u, "Remote Con.ID == echoed VAX Con.ID (abs 64)");
    check(le32(out + 68) == (SCS_CONNECT_OVMX_CONID_BASE | 0x0001u), "Local Con.ID == OVMX's (abs 68)");

    /* Live counters at the GROUNDED offsets -- NOT the golden 7/8. */
    check(le16(out + 32) == 17, "recv_ack [18:20] == live recv_seq 17 (abs 32), not golden 7");
    check(le16(out + 34) == 19, "send_seq [20:22] == live send_seq 19 (abs 34), not golden 8");
    check(le16(out + 36) == 3,  "incarnation [22:24] == echoed N=3 (abs 36)");
    check(le16(out + 40) == 17, "recv_ack mirror [26:28] == 17 (abs 40)");
    check(le16(out + 44) == 19, "send_seq mirror [30:32] == 19 (abs 44), GROUNDED [20:22]==[30:32]");
    check(le16(out + 48) == 17, "recv_ack 3rd repeat [34:36] == 17 (abs 48)");
    check(le16(out + 34) == le16(out + 44), "send-seq mirror holds ([20:22]==[30:32])");

    /* A zero incarnation leaves the template's fresh-contact value 1 (byte-exact
     * golden reproduction for the fresh-formation path). */
    struct scs_connect_params fresh = cp;
    fresh.incarnation = 0;
    fresh.recv_ack = 7;
    fresh.send_seq = 8;
    uint8_t out2[SCS_CONNECT_FRAME_LEN];
    check(scs_connect_build_response(&fresh, out2) == 0, "build_response (fresh) succeeds");
    check(le16(out2 + 36) == 1, "incarnation 0 leaves the fresh template value 1 (abs 36)");
    check(le16(out2 + 32) == 7 && le16(out2 + 34) == 8,
          "fresh-formation golden counters 7/8 reproduced when passed explicitly");
}

/*
 * vms-e1a, p. 2-35: "each packet contains source and destination CONIDs. These
 * quantities come from the CDT used by SCS on the source node to describe the
 * connection between the two SYSAPs. The source CONID comes from the local
 * CONID field of that CDT; and the destination CONID comes from the remote
 * CONID field of the CDT."
 *
 * The 110-byte connect class is where the two CONIDs are EXCHANGED (p. 2-28:
 * "SCA defines that these numbers be included in CONNECT_REQ and ACCEPT_REQ
 * packets"), so it is the one class with a principled zero: a CONNECT_REQ has
 * no destination CONID yet because the peer's CDT does not exist. Everything
 * after it carries both. This pins that asymmetry on the frames OVMX BUILDS,
 * and pins that whatever OVMX uses as its local CONID reaches abs 68 unaltered
 * -- which is what lets a CDT's local_conid field be the source of that value
 * without any wire change (see scs_cdt.h's WIRE VERDICT).
 */
static void test_both_conids_present_p235(void)
{
    printf("[p. 2-35: source and destination Con.IDs on the built frames]\n");
    struct scs_connect_params cp;
    memset(&cp, 0, sizeof(cp));
    memcpy(cp.dst_mac, vax1_mac, 6);
    memcpy(cp.src_mac, ovmx_mac, 6);
    memcpy(cp.src_logical, ovmx_logical, 6);
    memcpy(cp.peer_logical, vax1_mac, 6);

    /* An arbitrary CONID pair, deliberately NOT the shipped OVMX constants, so
     * the assertion is "the field is filled from the parameter", not "the field
     * happens to hold a familiar constant". */
    const uint32_t local = 0x4F58002Au;  /* would be CDL slot 0x2A */
    const uint32_t remote = 0x33580008u; /* the VAX's own CONID */
    cp.local_conid = local;
    cp.remote_conid = remote;

    uint8_t req[SCS_CONNECT_FRAME_LEN];
    uint8_t resp[SCS_CONNECT_FRAME_LEN];
    check(scs_connect_build_request(&cp, req) == 0, "build_request succeeds");
    check(scs_connect_build_response(&cp, resp) == 0, "build_response succeeds");

    /* CONNECT_REQ: source CONID present, destination CONID zero BY THE
     * ARCHITECTURE (the peer's CDT does not exist yet, p. 2-28). */
    check(le32(req + 68) == local, "CONNECT_REQ source Con.ID == our local CONID (abs 68)");
    check(le32(req + 64) == 0, "CONNECT_REQ destination Con.ID == 0 (peer CDT not yet formed)");

    /* ACCEPT_REQ / CONNECT-RESPONSE: BOTH present. */
    check(le32(resp + 68) == local, "ACCEPT_REQ source Con.ID == our local CONID (abs 68)");
    check(le32(resp + 64) == remote, "ACCEPT_REQ destination Con.ID == peer's CONID (abs 64)");
    check(le32(resp + 64) != 0 && le32(resp + 68) != 0,
          "ACCEPT_REQ carries BOTH Con.IDs non-zero (p. 2-35)");

    /* The parser reads back exactly what the builder wrote, both directions. */
    struct scs_connect_view v;
    check(scs_connect_parse(resp, sizeof(resp), &v) == 0, "parse our own ACCEPT_REQ");
    check(v.has_conid && v.local_conid == local && v.remote_conid == remote,
          "round-trip: parsed Con.ID pair matches what was built");
}

/*
 * vms-dd5: THE 0x4b CONNECT FRAMES ARE FIGURE 2-14 MESSAGES TOO.
 *
 * docs/cluster-protocol-spec.md sec 4(h)(1a) grounds the [46:48] field as the
 * SCA connection-control message type and shows the SAME four-message dialogue
 * running for five different SYSAPs, VMS$VAXcluster among them. This module's
 * two builders are therefore Figure 2-14's CONNECT_REQ and ACCEPT_REQ, and this
 * test asserts that the bytes they emit classify that way -- without a socket.
 *
 * NOTE WHAT IS *NOT* ASSERTED. OVMX builds no CONNECT_RSP and no ACCEPT_RSP for
 * this SYSAP. Both exist on the real wire (the 66-byte message-type-1 and the
 * 62-byte message-type-3 frames), so this is an OVMX gap, not a wire gap; the
 * state machine reports the missing sends as SCSD-W-CONNNOACT at runtime rather
 * than pretending they went out.
 */
static void test_connect_frames_classify_as_figure_2_14_messages(void)
{
    printf("[vms-dd5 Figure 2-14 classification of the 0x4b connect frames]\n");

    struct scs_connect_params p;
    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, vax1_mac, 6);
    memcpy(p.src_mac, ovmx_mac, 6);
    memcpy(p.src_logical, ovmx_logical, 6);
    memcpy(p.peer_logical, vax1_mac, 6);
    p.local_conid = SCS_CONNECT_OVMX_CONID_BASE | 0x0002u;
    p.remote_conid = 0x33580008u;

    uint8_t req[SCS_CONNECT_FRAME_LEN];
    check(scs_connect_build_request(&p, req) == 0, "build the 0x4b CONNECT-REQUEST");
    /* [46:48] is absolute 60:62. */
    unsigned req_msgtype = (unsigned)req[60] | ((unsigned)req[61] << 8);
    check(req_msgtype == 0, "the CONNECT-REQUEST carries connection-control message type 0");
    struct scs_connect_view rv;
    check(scs_connect_parse(req, sizeof(req), &rv) == 0, "parse it back");
    check(rv.remote_conid == 0 && rv.local_conid == p.local_conid,
          "the CONNECT-REQUEST has the CONNECT_REQ Con.ID shape (destination 0)");
    enum scs_conn_event ev;
    check(scs_conn_event_for_msgtype(req_msgtype, &ev) == 1, "message type 0 is mapped");
    check(ev == SCS_CONN_EV_RCV_CONNECT_REQ, "message type 0 maps to RCV_CONNECT_REQ");

    uint8_t rsp[SCS_CONNECT_FRAME_LEN];
    check(scs_connect_build_response(&p, rsp) == 0, "build the 0x4b CONNECT-RESPONSE");
    unsigned rsp_msgtype = (unsigned)rsp[60] | ((unsigned)rsp[61] << 8);
    check(rsp_msgtype == 2, "the CONNECT-RESPONSE carries connection-control message type 2");
    check(scs_connect_parse(rsp, sizeof(rsp), &rv) == 0, "parse it back");
    check(rv.remote_conid == p.remote_conid && rv.local_conid == p.local_conid,
          "the CONNECT-RESPONSE has the ACCEPT_REQ shape (both Con.IDs supplied)");
    check(scs_conn_event_for_msgtype(rsp_msgtype, &ev) == 1, "message type 2 is mapped");
    check(ev == SCS_CONN_EV_RCV_ACCEPT_REQ, "message type 2 maps to RCV_ACCEPT_REQ");

    /* The source's Figure 2-14 column, driven by those two message types plus
     * the CONNECT_RSP the peer sends between them. */
    static struct scs_cdl cdl;
    scs_cdl_init(&cdl);
    struct scs_cdt *c = scs_cdl_alloc_conid(&cdl, p.local_conid, "VMS$VAXcluster",
                                            "VMS$VAXcluster", NULL);
    check(c != NULL, "allocate the joiner CDT at the Con.ID we put on the wire");
    if (c == NULL) {
        return;
    }
    scs_conn_fsm_init(c);
    struct scs_conn_transition t = scs_conn_fsm_step(c, SCS_CONN_EV_SVC_CONNECT);
    check(t.action == SCS_CONN_ACT_SEND_CONNECT_REQ && t.to == SCS_CONN_CONNECT_SENT,
          "invoking CONNECT asks for the frame this module builds first");
    t = scs_conn_fsm_step(c, SCS_CONN_EV_RCV_CONNECT_RSP);
    check(t.to == SCS_CONN_CONNECT_ACK && t.documented,
          "the peer's message-type-1 CONNECT_RSP advances to CONNECT ACK by the book");
    t = scs_conn_fsm_step(c, SCS_CONN_EV_RCV_ACCEPT_REQ);
    check(t.to == SCS_CONN_OPEN && t.documented,
          "the peer's message-type-2 ACCEPT_REQ opens the connection by the book");
    check(t.action == SCS_CONN_ACT_SEND_ACCEPT_RSP,
          "and the book requires an ACCEPT_RSP that this module has no builder for");
}

int main(void)
{
    printf("test_scs_connect: directed HELLO + SCS connect (vms-5fe/vms-c6d)\n");
    test_directed_hello();
    test_parse_real_frames();
    test_build_request();
    test_build_response();
    test_response_live_counters();
    test_both_conids_present_p235();
    test_connect_frames_classify_as_figure_2_14_messages();
    printf("test_scs_connect: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
