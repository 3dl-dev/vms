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

/* OVMX test identity. */
static const uint8_t ovmx_mac[6] = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 }; /* "OVX", local bit */
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
    strncpy(p.node_name, "OVMX", sizeof(p.node_name) - 1);
    p.timer_tick = 0;

    static const uint8_t nonce[4] = SCS_HELLO_LAB_NONCE_BYTES;
    uint8_t out[SCS_HELLO_FRAME_LEN];
    memset(out, 0xAA, sizeof(out));
    int rc = scs_hello_build_directed_frame(&p, vax1_mac, nonce, 1, out);
    check(rc == 0, "scs_hello_build_directed_frame succeeds");

    /* Directed frame is addressed to the PEER (abs 0), and abs 16 mirrors it. */
    check_bytes(out + 0, vax1_mac, 6, "Ethernet dst == peer MAC (directed, not multicast)");
    check_bytes(out + 16, vax1_mac, 6, "SCA dest logical addr mirrors peer MAC (abs 16)");
    check_bytes(out + 6, ovmx_mac, 6, "Ethernet src == OVMX HW MAC");
    check_bytes(out + 24, ovmx_mac, 6, "SCA src logical addr == OVMX HW MAC (abs 24)");

    /* GROUNDED directed markers (spec sec 4b). */
    static const uint8_t nonce_want[4] = SCS_HELLO_LAB_NONCE_BYTES;
    check_bytes(out + 68, nonce_want, 4, "join nonce == ee-05-39-5b (GROUNDED present, REPLAYED value)");
    check(out[92] == 0x01 && out[93] == 0x00,
          "directed-HELLO flag / incarnation == 0x0001 for a fresh contact (GROUNDED, spec 4b/4i.B)");
    check(out[128] == 0x1f && out[129] == 0x00, "poller-sweep marker == 0x001f=31 (GROUNDED)");
    /* per-frame word: directed replay value (ungrounded). */
    check(out[30] == 0xb3 && out[31] == 0x00, "per-frame word == 0xb300 (directed, REPLAYED)");

    /* Length + node name unchanged from the multicast template. */
    check(out[14] == 0x76 && out[15] == 0x00, "SCA length field == 0x0076 (total 120)");
    check(out[40] == 6, "node-name length prefix == 6");
    check_bytes(out + 41, (const uint8_t *)"OVMX  ", 6, "node name == 'OVMX  '");

    check(scs_hello_build_directed_frame(NULL, vax1_mac, nonce, 1, out) == -1, "NULL params rejected");
    check(scs_hello_build_directed_frame(&p, NULL, nonce, 1, out) == -1, "NULL peer rejected");
    check(scs_hello_build_directed_frame(&p, vax1_mac, NULL, 1, out) == -1, "NULL nonce rejected");
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
    check_bytes(out + 24, ovmx_mac, 6, "SCA src logical == OVMX HW MAC (abs 24)");
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

int main(void)
{
    printf("test_scs_connect: directed HELLO + SCS connect (vms-5fe)\n");
    test_directed_hello();
    test_parse_real_frames();
    test_build_request();
    test_build_response();
    printf("test_scs_connect: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
