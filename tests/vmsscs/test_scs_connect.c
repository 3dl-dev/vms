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
#include <stdlib.h>
#include <string.h>

#include "scs_conn.h" /* vms-dd5: the connection state machine + wire->event map */
#include "scs_connect.h"
#include "scs_env.h" /* vms-a61: builds the envelope-conformant test fixture directly */
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

/* ---------------------------------------------------------------------------
 * vms-fdd: THE ESTABLISHED-JOIN SPECIMEN.
 *
 * vax3-2to3-established-join-20260730.pcap is the only capture in the library
 * of a REAL node being admitted to an already-running cluster (spec sec 1),
 * which is the operation OVMX performs. These are two of its frames, dumped
 * byte-for-byte:
 *
 *   raw frame 132 -- VAX3 (the JOINER, 08:00:2b:11:22:33) -> VAX1,
 *                    VMS$VAXcluster CONNECT_REQ (message type 0)
 *   raw frame 136 -- VAX1 (an established MEMBER) -> VAX3,
 *                    VMS$VAXcluster ACCEPT_REQ (message type 2)
 *
 * They carry DIFFERENT connect data, which is what makes the decode test a
 * test: a decoder that returned a constant, or read the wrong 16 bytes, cannot
 * produce both. Re-derive with tools/scs_connect_data_measure.py.
 * ------------------------------------------------------------------------- */
static const uint8_t vax3_joiner_connect_req[124] = {
    0xaa,0x00,0x04,0x00,0x01,0x04, 0x08,0x00,0x2b,0x11,0x22,0x33, 0x60,0x07, 0x6c,0x00,
    0xaa,0x00,0x04,0x00,0x01,0x04, 0x01,0x00, 0xaa,0x00,0x04,0x00,0x03,0x04, 0x5b,0x13,
    0x08,0x00,0x0a,0x00,0x01,0x00,0x12,0x00, 0x08,0x00,0x00,0x00,0x0a,0x00,0x00,0x00,
    0x08,0x00,0x00,0x00,0x01,0x00,0x00,0x02, 0x42,0x00,0x04,0x00,0x00,0x00,0x0a,0x00,
    0x00,0x00,0x00,0x00, 0x09,0x00,0xe3,0x18, 0x00,0x00,0x01,0x00, 0x56,0x4d,0x53,0x24,
    0x56,0x41,0x58,0x63,0x6c,0x75,0x73,0x74,0x65,0x72,0x20,0x20, 0x56,0x4d,0x53,0x24,
    0x56,0x41,0x58,0x63,0x6c,0x75,0x73,0x74,0x65,0x72,0x20,0x20, 0x01,0x1b,0x01,0x03,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x06,0x00
};
static const uint8_t vax1_member_accept_req[124] = {
    0x08,0x00,0x2b,0x11,0x22,0x33, 0xaa,0x00,0x04,0x00,0x01,0x04, 0x60,0x07, 0x6c,0x00,
    0xaa,0x00,0x04,0x00,0x03,0x04, 0x01,0x00, 0xaa,0x00,0x04,0x00,0x01,0x04, 0x4b,0x13,
    0x0a,0x00,0x0b,0x00,0x01,0x00,0x12,0x00, 0x0a,0x00,0x00,0x00,0x0b,0x00,0x00,0x00,
    0x0a,0x00,0x00,0x00,0x01,0x00,0x00,0x02, 0x42,0x00,0x04,0x00,0x02,0x00,0x0a,0x00,
    0x09,0x00,0xe3,0x18, 0x0e,0x00,0x52,0x35, 0x00,0x00,0x00,0x00, 0x56,0x4d,0x53,0x24,
    0x56,0x41,0x58,0x63,0x6c,0x75,0x73,0x74,0x65,0x72,0x20,0x20, 0x56,0x4d,0x53,0x24,
    0x56,0x41,0x58,0x63,0x6c,0x75,0x73,0x74,0x65,0x72,0x20,0x20, 0x01,0x1b,0x01,0x03,
    0x01,0x00,0x01,0x00,0x02,0x00,0x01,0x08,0x00,0x00,0x06,0x00
};

/* The two connect-data values, spelled out independently of the production
 * constant so a mutation of scs_connect_data_vaxcluster[] reds this file. */
static const uint8_t joiner_cd[SCS_CONNECT_DATA_LEN] = {
    0x01,0x1b,0x01,0x03, 0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x08,0x00,0x00,0x06,0x00
};
static const uint8_t member_cd[SCS_CONNECT_DATA_LEN] = {
    0x01,0x1b,0x01,0x03, 0x01,0x00,0x01,0x00,0x02,0x00,0x01, 0x08,0x00,0x00,0x06,0x00
};

static void fill_params(struct scs_connect_params *cp)
{
    memset(cp, 0, sizeof(*cp));
    memcpy(cp->dst_mac, vax1_mac, 6);
    memcpy(cp->src_mac, ovmx_mac, 6);
    memcpy(cp->src_logical, ovmx_logical, 6);
    memcpy(cp->peer_logical, vax1_mac, 6);
    cp->local_conid = SCS_CONNECT_OVMX_CONID_BASE | 0x0001u;
    cp->remote_conid = 0x62C50009u;
}

/*
 * vms-fdd (1): the 16-byte connect data both builders put on the wire, asserted
 * byte-for-byte at the GROUNDED offset (abs 108-123 = payload [94:110]).
 */
static void test_connect_data_byte_exact_in_both_builders(void)
{
    printf("[connect data: byte-exact in both builders]\n");
    scs_connect_data_reset_switch_cache();

    struct scs_connect_params cp;
    uint8_t req[SCS_CONNECT_FRAME_LEN];
    uint8_t rsp[SCS_CONNECT_FRAME_LEN];
    fill_params(&cp);
    check(scs_connect_build_request(&cp, req) == 0, "build_request succeeds");
    check(scs_connect_build_response(&cp, rsp) == 0, "build_response succeeds");

    check(SCS_CONNECT_DATA_ABS_OFF == 108, "connect data sits at abs 108 (payload [94:110])");
    check(SCS_CONNECT_DATA_LEN == 16, "connect data is 16 bytes (p. 2-25 'up to 16')");

    check_bytes(req + SCS_CONNECT_DATA_ABS_OFF, joiner_cd, SCS_CONNECT_DATA_LEN,
                "CONNECT_REQ connect data == the joiner value from raw frame 132");
    check_bytes(rsp + SCS_CONNECT_DATA_ABS_OFF, joiner_cd, SCS_CONNECT_DATA_LEN,
                "ACCEPT_REQ connect data == the joiner value from raw frame 210");
    check_bytes(scs_connect_data_vaxcluster, joiner_cd, SCS_CONNECT_DATA_LEN,
                "the exported constant is that same measured value");

    /* It is the LAST 16 bytes: nothing follows, and the SYSAP name field
     * immediately before it is untouched (spec sec 4h(2)). */
    check(SCS_CONNECT_DATA_ABS_OFF + SCS_CONNECT_DATA_LEN == SCS_CONNECT_FRAME_LEN,
          "connect data is the frame's last 16 bytes");
    check_bytes(req + 92, (const uint8_t *)"VMS$VAXcluster  ", 16,
                "the remote SYSAP name field just before it is unchanged");

    /* And the stamp changed nothing else: the request differs from a
     * pre-vms-fdd build ONLY inside the connect data. */
    uint8_t before[SCS_CONNECT_FRAME_LEN];
    setenv("OVMX_NO_CONNECT_DATA", "1", 1);
    scs_connect_data_reset_switch_cache();
    check(scs_connect_build_request(&cp, before) == 0, "build_request with the stamp off succeeds");
    unsetenv("OVMX_NO_CONNECT_DATA");
    scs_connect_data_reset_switch_cache();
    check(memcmp(before, req, SCS_CONNECT_DATA_ABS_OFF) == 0,
          "every byte BEFORE the connect data is identical with the stamp on and off");
}

/*
 * vms-fdd (2): THE KILL SWITCH, RUN (guardrail 23). OVMX_NO_CONNECT_DATA=1
 * must actually suppress the stamp, and the suppressed frame must carry the
 * captured template's own bytes -- which for the CONNECT-REQUEST is VAX1's, a
 * MEMBER's, connect data. If the switch gated nothing, the two builds would be
 * equal and the inequality assertion below would fail.
 */
static void test_connect_data_kill_switch(void)
{
    printf("[connect data: OVMX_NO_CONNECT_DATA=1 kill switch]\n");
    struct scs_connect_params cp;
    fill_params(&cp);

    uint8_t on_req[SCS_CONNECT_FRAME_LEN], off_req[SCS_CONNECT_FRAME_LEN];
    uint8_t on_rsp[SCS_CONNECT_FRAME_LEN], off_rsp[SCS_CONNECT_FRAME_LEN];

    unsetenv("OVMX_NO_CONNECT_DATA");
    scs_connect_data_reset_switch_cache();
    check(scs_connect_data_enabled() == 1, "stamp is ON with the variable unset");
    scs_connect_build_request(&cp, on_req);
    scs_connect_build_response(&cp, on_rsp);

    setenv("OVMX_NO_CONNECT_DATA", "1", 1);
    scs_connect_data_reset_switch_cache();
    check(scs_connect_data_enabled() == 0, "stamp is OFF with OVMX_NO_CONNECT_DATA=1");
    scs_connect_build_request(&cp, off_req);
    scs_connect_build_response(&cp, off_rsp);

    /* THE GATE IS REAL: the request's 16 bytes actually change. */
    check(memcmp(on_req + SCS_CONNECT_DATA_ABS_OFF,
                 off_req + SCS_CONNECT_DATA_ABS_OFF, SCS_CONNECT_DATA_LEN) != 0,
          "the switch CHANGES the CONNECT_REQ connect data (it gates a real byte)");
    /* The fallback is the CONNECT-REQUEST template's own bytes, i.e. the
     * golden capture's VAX1 frame (raw 47) -- an established MEMBER's connect
     * data, and one of the 5 VMS$VAXcluster values in the census. Asserted
     * against that captured frame rather than a re-typed literal. */
    check_bytes(off_req + SCS_CONNECT_DATA_ABS_OFF,
                real_request + SCS_CONNECT_DATA_ABS_OFF, SCS_CONNECT_DATA_LEN,
                "with the stamp off the CONNECT_REQ falls back to the golden template's MEMBER value");
    check(memcmp(real_request + SCS_CONNECT_DATA_ABS_OFF, joiner_cd,
                 SCS_CONNECT_DATA_LEN) != 0,
          "and that template value is NOT the joiner value (which is why the stamp exists)");
    check(memcmp(on_req, off_req, SCS_CONNECT_FRAME_LEN - SCS_CONNECT_DATA_LEN) == 0,
          "and the switch changes NOTHING outside the connect data");

    /* Bracketing control: the RESPONSE template is a joiner's frame, so it
     * already carried the stamped value -- the switch is a no-op there, and
     * saying so is the honest scope of the change. */
    check(memcmp(on_rsp, off_rsp, SCS_CONNECT_FRAME_LEN) == 0,
          "the CONNECT-RESPONSE is byte-identical either way (its template was already the joiner value)");

    /* A value other than exactly "1" does NOT disable the stamp. */
    setenv("OVMX_NO_CONNECT_DATA", "0", 1);
    scs_connect_data_reset_switch_cache();
    check(scs_connect_data_enabled() == 1, "OVMX_NO_CONNECT_DATA=0 leaves the stamp ON");
    setenv("OVMX_NO_CONNECT_DATA", "11", 1);
    scs_connect_data_reset_switch_cache();
    check(scs_connect_data_enabled() == 1, "OVMX_NO_CONNECT_DATA=11 leaves the stamp ON");

    unsetenv("OVMX_NO_CONNECT_DATA");
    scs_connect_data_reset_switch_cache();
}

/*
 * vms-fdd (3): DECODE AGAINST REAL CAPTURED FRAMES. Two frames of the
 * established-join specimen, carrying two DIFFERENT connect-data values, plus
 * the negative cases the field is NOT claimed for.
 */
static void test_connect_data_decode_real_frames(void)
{
    printf("[connect data: decode real captured frames]\n");
    uint8_t cd[SCS_CONNECT_DATA_LEN];
    struct scs_connect_view v;

    /* The joiner's CONNECT_REQ (raw 132). */
    check(scs_connect_data_get(vax3_joiner_connect_req,
                               sizeof(vax3_joiner_connect_req), cd) == 0,
          "decode VAX3's real CONNECT_REQ (raw frame 132)");
    check_bytes(cd, joiner_cd, SCS_CONNECT_DATA_LEN,
                "VAX3's connect data decodes to the joiner value");
    check(scs_connect_parse(vax3_joiner_connect_req,
                            sizeof(vax3_joiner_connect_req), &v) == 0, "parse raw 132");
    check(v.has_connect_data == 1, "parse marks raw 132 as carrying connect data");
    check(v.conn_msgtype == SCS_CONN_MSGTYPE_CONNECT_REQ, "raw 132 message type == 0 CONNECT_REQ");
    check_bytes(v.connect_data, joiner_cd, SCS_CONNECT_DATA_LEN, "view carries the joiner value");

    /* The member's ACCEPT_REQ (raw 136) -- a DIFFERENT value. */
    check(scs_connect_data_get(vax1_member_accept_req,
                               sizeof(vax1_member_accept_req), cd) == 0,
          "decode VAX1's real ACCEPT_REQ (raw frame 136)");
    check_bytes(cd, member_cd, SCS_CONNECT_DATA_LEN,
                "VAX1's connect data decodes to the MEMBER value, not the joiner value");
    check(memcmp(joiner_cd, member_cd, SCS_CONNECT_DATA_LEN) != 0,
          "the two captured values really are different (the decode test discriminates)");
    check(scs_connect_parse(vax1_member_accept_req,
                            sizeof(vax1_member_accept_req), &v) == 0, "parse raw 136");
    check(v.conn_msgtype == SCS_CONN_MSGTYPE_ACCEPT_REQ, "raw 136 message type == 2 ACCEPT_REQ");
    check_bytes(v.connect_data, member_cd, SCS_CONNECT_DATA_LEN, "view carries the member value");

    /* The two invariant spans, read off the captured frames rather than off
     * our own builder (148/148 VAX-sourced frames across the library; OVMX's
     * own 55 are excluded from the census -- see the guard in scs_connect.h). */
    check(memcmp(vax3_joiner_connect_req + SCS_CONNECT_DATA_ABS_OFF,
                 vax1_member_accept_req + SCS_CONNECT_DATA_ABS_OFF, 4) == 0,
          "the [94:98] version quad is the same in both captured frames");
    check(memcmp(vax3_joiner_connect_req + SCS_CONNECT_DATA_ABS_OFF + 11,
                 vax1_member_accept_req + SCS_CONNECT_DATA_ABS_OFF + 11, 5) == 0,
          "the [105:110] tail is the same in both captured frames");
    check(memcmp(vax3_joiner_connect_req + SCS_CONNECT_DATA_ABS_OFF + 4,
                 vax1_member_accept_req + SCS_CONNECT_DATA_ABS_OFF + 4, 7) != 0,
          "and [98:105] is what differs between joiner and member (the RE gap)");

    /* NEGATIVES -- the field is claimed for message types 0 and 2 only. */
    uint8_t mangled[124];
    memcpy(mangled, vax3_joiner_connect_req, sizeof(mangled));
    mangled[60] = 10; /* the 110-byte class's OTHER message type (2889 frames) */
    check(scs_connect_data_get(mangled, sizeof(mangled), cd) == -1,
          "message type 10 in the same length class is REFUSED (not a connect frame)");
    memcpy(mangled, vax3_joiner_connect_req, sizeof(mangled));
    mangled[60] = 1; /* CONNECT_RSP: real, but a 66-byte frame with no such field */
    check(scs_connect_data_get(mangled, sizeof(mangled), cd) == -1,
          "message type 1 (CONNECT_RSP) is REFUSED");
    memcpy(mangled, vax3_joiner_connect_req, sizeof(mangled));
    mangled[31] = 0x14; /* not the GROUNDED format constant */
    check(scs_connect_data_get(mangled, sizeof(mangled), cd) == -1,
          "a frame whose format byte is not 0x13 is REFUSED");
    memcpy(mangled, vax3_joiner_connect_req, sizeof(mangled));
    mangled[14] = 0xbc; /* SCA length word -> 190-byte class */
    check(scs_connect_data_get(mangled, sizeof(mangled), cd) == -1,
          "the 190-byte VC class is REFUSED (no connect data there)");
    check(scs_connect_data_get(vax3_joiner_connect_req, 100, cd) == -1,
          "a truncated frame is REFUSED");
    check(scs_connect_data_get(NULL, 124, cd) == -1, "NULL frame is REFUSED");
    check(scs_connect_data_get(vax3_joiner_connect_req, 124, NULL) == -1, "NULL out is REFUSED");

    /* And a frame with no connect data leaves the view's flag clear. */
    uint8_t shortframe[64];
    memset(shortframe, 0, sizeof(shortframe));
    shortframe[14] = 0x27; /* 41-byte credit-return class */
    shortframe[30] = SCS_MSGTYPE_CREDIT;
    shortframe[31] = SCS_FORMAT_CONST;
    check(scs_connect_parse(shortframe, sizeof(shortframe), &v) == 0, "parse a credit short");
    check(v.has_connect_data == 0, "a 0x48 credit short carries no connect data");

    /* The log renderer used by scsd.c. */
    char buf[80];
    const char *s = scs_connect_data_fmt(joiner_cd, buf, sizeof(buf));
    check(strncmp(s, "01 1b 01 03 ", 12) == 0, "fmt renders the version quad first");
    check(strstr(s, "|") != NULL, "fmt renders an ASCII column");
    check(strcmp(scs_connect_data_fmt(joiner_cd, buf, 8), "") == 0,
          "fmt refuses a buffer too small to hold the rendering");
}

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

/*
 * vms-a61: has_conid used to be gated on `total_sca_len == 110 || 190` --
 * two of the SEVEN envelope-conformant length classes (scs_env.h: 58, 62, 66,
 * 86, 94, 110, 190) -- even though the Con.ID pair sits at the SAME fixed
 * content[50:58] offset on every one of them. This proves the widened gate
 * (scs_env_parse_frame's conformance test) admits the SHORTEST class, 58,
 * which the old length pair refused outright.
 *
 * THE INPUT IS BUILT BY THE PRODUCTION ENVELOPE BUILDER, not hand-typed: a
 * hand-typed "58-byte conformant frame" could easily fail the conformance
 * test by accident (get the inner length or format word wrong) and this test
 * would then be proving nothing. scs_env_build_frame() is scs_env.c's own
 * builder -- the same one scs_env_measure.py part (D) proves byte-exact
 * against 319,575+ real captured frames -- so a successful build is already
 * proof the frame is conformant before scs_connect_parse ever sees it.
 */
static void test_has_conid_widens_past_the_old_110_190_special_case(void)
{
    printf("[has_conid: widened to every envelope-conformant class, not just 110/190]\n");

    uint8_t frame[14 + SCS_ENV_HDR_END];
    memset(frame, 0, sizeof(frame));
    frame[12] = 0x60;
    frame[13] = 0x07; /* SCA ethertype */
    uint8_t *content = frame + 14;
    /* scs_env_build() deliberately does NOT write the [0:2] SCA length word
     * (scs_env.h: "it belongs to the frame class, not to the SCS envelope") --
     * the caller must, exactly as every real builder in this tree does. */
    uint16_t lenword = (uint16_t)(SCS_ENV_HDR_END - 2);
    content[0] = (uint8_t)(lenword & 0xffu);
    content[1] = (uint8_t)((lenword >> 8) & 0xffu);

    struct scs_env_fields f;
    f.mtype = SCS_ENV_MTYPE_DISCONNECT_REQ;
    f.credit = 0;
    f.dest_conid = 0xAABBCCDDu;
    f.src_conid = 0x11223344u;
    check(scs_env_build_frame(frame, sizeof(frame), &f) == 0,
          "scs_env_build_frame built the 58-content envelope-only fixture");

    struct scs_connect_view v;
    check(scs_connect_parse(frame, sizeof(frame), &v) == 0, "parse the 58-content frame");
    check(v.total_sca_len == 58, "the fixture really is the 58-content class");
    check(v.has_conid == 1,
          "58-content class now carries has_conid -- the OLD code set it ONLY"
          " for total_sca_len == 110 or 190 and would have refused this exact"
          " class outright");
    check(v.remote_conid == 0xAABBCCDDu, "remote Con.ID reads back the built dest_conid");
    check(v.local_conid == 0x11223344u, "local Con.ID reads back the built src_conid");
    check(v.conn_msgtype == SCS_ENV_MTYPE_DISCONNECT_REQ,
          "conn_msgtype reads back the built MTYPE off the parsed envelope,"
          " not an unguarded raw offset read (vms-a61's scs_connect.c:304 fix)");

    /* NEGATIVE CONTROL: a frame that fails the conformance test (bad format
     * word) must still get has_conid == 0 -- the widening is to "every
     * conformant class", not to "every frame this long". */
    uint8_t bad[sizeof(frame)];
    memcpy(bad, frame, sizeof(bad));
    bad[14 + 44] ^= 0xFFu; /* corrupt the format word at content[44] */
    struct scs_connect_view bv;
    check(scs_connect_parse(bad, sizeof(bad), &bv) == 0, "parse still succeeds (refusal is in the flag, not the return)");
    check(bv.has_conid == 0,
          "a non-conformant frame of the SAME length must NOT get has_conid --"
          " the gate is conformance, not length");
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

/* =====================================================================
 * vms-578 INTEGRATION: test functions added by worktree-760.
 * Both branches only APPENDED here, so nothing is replaced -- these are
 * carried over verbatim and registered in main() below.
 * ===================================================================== */


/* vms-760: byte-exact 110-byte SCA content of the clean 1->2-node formation
 * joiner's MSCP$DISK client CONNECT-REQUEST (formation-clean-2node.pcap SCA
 * idx35, joiner 08:00:2b:94:ca:47 -> member). Building with this frame's exact
 * identity/Con.ID/seq must reproduce it byte-for-byte -- the proof that the
 * MSCP$DISK connect builder is a faithful replay, not a hand-rolled guess. */
static const uint8_t clean_mscp_request_sca[110] = {
    0x6c,0x00, 0xaa,0x00,0x04,0x00,0x4c,0x04, 0xe8,0x03, 0xaa,0x00,0x04,0x00,0x4d,0x04,
    0x5b,0x13, 0x04,0x00,0x06,0x00,0x01,0x00,0x12,0x00, 0x04,0x00,0x00,0x00,0x06,0x00,0x00,0x00,
    0x04,0x00,0x00,0x00,0x01,0x00,0x00,0x02, 0x42,0x00,0x04,0x00,0x00,0x00,0x0a,0x00,
    0x00,0x00,0x00,0x00, 0x08,0x00,0x62,0x4e, 0x02,0x00,0x01,0x00,
    'M','S','C','P','$','D','I','S','K',' ',' ',' ',' ',' ',' ',' ',
    'V','M','S','$','D','I','S','K','_','C','L','_','D','R','V','R',
    'V','5','.','0',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ','+',' '
};

static void test_build_mscp_request(void)
{
    printf("[build MSCP$DISK CONNECT-REQUEST (vms-760)]\n");
    /* The clean-ref joiner's exact identity/Con.ID/seq for SCA idx35. */
    static const uint8_t clean_dest_logical[6] = { 0xaa,0x00,0x04,0x00,0x4c,0x04 };
    static const uint8_t clean_src_logical[6]  = { 0xaa,0x00,0x04,0x00,0x4d,0x04 };
    struct scs_connect_params cp;
    memset(&cp, 0, sizeof(cp));
    memcpy(cp.dst_mac, clean_dest_logical, 6);
    memcpy(cp.src_mac, ovmx_mac, 6);          /* Ethernet src (not part of SCA) */
    memcpy(cp.src_logical, clean_src_logical, 6);
    memcpy(cp.peer_logical, clean_dest_logical, 6);
    cp.local_conid = 0x4e620008u;             /* clean joiner's MSCP handle */
    cp.remote_conid = 0;                      /* ignored (REQUEST forces 0) */
    cp.recv_ack = 4;                          /* clean idx35 [18:20] */
    cp.send_seq = 6;                          /* clean idx35 [20:22] */
    cp.incarnation = 1;                       /* clean idx35 [22:24] */

    uint8_t out[SCS_CONNECT_FRAME_LEN];
    memset(out, 0xAA, sizeof(out));
    check(scs_connect_build_mscp_request(&cp, out) == 0, "build_mscp_request succeeds");

    /* The SCA content (abs 14..123) reproduces the captured idx35 byte-for-byte. */
    check_bytes(out + 14, clean_mscp_request_sca, 110,
                "SCA content == clean-ref idx35 byte-exact (identity/Con.ID/seq threaded)");

    /* Spot the load-bearing fields (also covered by the byte-exact check, asserted
     * explicitly for readable failure output). */
    check(out[30] == 0x5b && out[31] == 0x13,
          "msgtype 0x5b (MSCP connect, NOT the VC's 0x4b), format 0x13 (abs 30/31)");
    check_bytes(out + 76, (const uint8_t *)"MSCP$DISK       ", 16,
                "target SYSAP name == 'MSCP$DISK' (abs 76)");
    check_bytes(out + 92, (const uint8_t *)"VMS$DISK_CL_DRVR", 16,
                "requesting SYSAP name == 'VMS$DISK_CL_DRVR' (abs 92)");
    check_bytes(out + 108, (const uint8_t *)"V5.0          + ", 16,
                "class descriptor == 'V5.0          + ' (abs 108)");
    check(le32(out + 64) == 0x00000000u, "Remote Con.ID == 0 (CONNECT-REQUEST, abs 64)");
    check(le32(out + 68) == 0x4e620008u, "Local Con.ID == joiner MSCP handle (abs 68)");

    /* Con.ID + seq substitution proven independent of the template (build with
     * OVMX's live values). */
    struct scs_connect_params live = cp;
    live.local_conid = SCS_CONNECT_OVMX_CONID_BASE | 0x000Au; /* OVMX_MSCP_CONID */
    live.recv_ack = 3; live.send_seq = 9; live.incarnation = 0;
    uint8_t out2[SCS_CONNECT_FRAME_LEN];
    check(scs_connect_build_mscp_request(&live, out2) == 0, "build_mscp_request (live values) succeeds");
    check(le32(out2 + 68) == (SCS_CONNECT_OVMX_CONID_BASE | 0x000Au),
          "Local Con.ID threaded to OVMX_MSCP_CONID (abs 68)");
    check(le16(out2 + 32) == 3 && le16(out2 + 34) == 9,
          "live recv_ack/send_seq threaded (abs 32/34), not the golden 4/6");
    check(le16(out2 + 36) == 1, "incarnation 0 leaves the fresh template value 1 (abs 36)");
    /* SYSAP names unchanged by the seq/Con.ID substitution. */
    check_bytes(out2 + 76, (const uint8_t *)"MSCP$DISK       ", 16, "names survive live substitution");

    check(scs_connect_build_mscp_request(NULL, out) == -1, "build_mscp_request NULL params rejected");
    check(scs_connect_build_mscp_request(&cp, NULL) == -1, "build_mscp_request NULL out rejected");
}

int main(void)
{
    printf("test_scs_connect: directed HELLO + SCS connect (vms-5fe/vms-c6d)\n");
    test_directed_hello();
    test_parse_real_frames();
    test_has_conid_widens_past_the_old_110_190_special_case();
    test_build_request();
    test_build_response();
    test_response_live_counters();
    test_both_conids_present_p235();
    test_connect_frames_classify_as_figure_2_14_messages();
    test_connect_data_byte_exact_in_both_builders();
    test_connect_data_kill_switch();
    test_connect_data_decode_real_frames();
    /* vms-578: worktree-760 test functions, registered here too. */
    test_build_mscp_request();
    printf("test_scs_connect: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
