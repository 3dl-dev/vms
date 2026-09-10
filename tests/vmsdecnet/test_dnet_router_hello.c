/*
 * test_dnet_router_hello.c - self round-trip test for the DECnet Phase IV
 *                            Ethernet Router Hello codec (rd vms-0aba, extended
 *                            for the RSLIST tail, rd vms-df5).
 *
 * ORACLE-GROUNDED (rd vms-df5). A REAL OpenVMS VAX V7.3 router-hello was
 * captured on the lab segment (docs/decnet-provenance-register.md sec 4.6a):
 * the lone-L1-router (n = 0) routing message from source 1.2 is the exact
 * 27 bytes below. This test therefore:
 *   (a) DECODES the real captured specimen and asserts every field -- including
 *       the RSLIST tail (RSLIST-len = 8, reserved Name = 7 zeros, RSLIST-count
 *       = 0) that a conformant Phase IV router hello carries after MPD;
 *   (b) re-ENCODES the decoded message and asserts it is BYTE-IDENTICAL to the
 *       captured specimen -- i.e. OVMX's encoder reproduces exactly what a real
 *       VMS VAX put on the wire (an oracle round-trip, not merely a self one);
 *   (c) round-trips a message WITH one 7-byte router-list entry (n = 1), to
 *       prove the variable RSLIST entries survive encode->decode too.
 *
 * The n = 0 case is the one OVMX actually emits (a lone L1 router with no other
 * routers reported): a 27-byte routing message, DATA LENGTH = 0x1b. That
 * mandatory RSLIST tail is exactly what an earlier 18-byte OVMX frame omitted,
 * which a real OpenVMS VAX V7.3 rejected on the wire with routing event 4.4 and
 * never selected OVMX as designated router (rd vms-df5). The router-list ENTRY
 * bytes in the n = 1 vector are arbitrary opaque filler (OVMX carries entries
 * uninterpreted, per dnet_router_hello.h) -- NOT a claim about real DECnet
 * router-list wire content.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dnet_hello.h"
#include "dnet_router_hello.h"

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

/*
 * The REAL captured OpenVMS VAX V7.3 lone-L1-router (n = 0) router-hello,
 * source 1.2 (docs/decnet-provenance-register.md sec 4.6a). The 27-byte routing
 * message is verbatim; bytes past off 28 are the Ethernet-minimum zero pad the
 * data link adds (46-byte minimum) and are not part of the routing message.
 *
 *   off 0    msglen LE       0x001b (27 = the fixed message incl. the n=0 tail)
 *   off 2    rflags          0x0b   (control, router hello, msg type 5)
 *   off 3    version         0x02
 *   off 4    eco             0x00
 *   off 5    user_eco        0x00
 *   off 6    id              aa:00:04:00:02:04  (node 1.2)
 *   off 12   iinfo           0x02   (L1 router, low 2 bits = node type 2)
 *   off 13   blksize LE      0x05da (1498)
 *   off 15   priority        0x40   (64)
 *   off 16   area            0x00
 *   off 17   timer LE        0x000f (15)
 *   off 19   mpd             0x00
 *   off 20   rslist_len      0x08   (8 + 0 router-list bytes)
 *   off 21   name[7]         all zero (reserved)
 *   off 28   rslist_count    0x00   (no router-list bytes)
 *   off 29   pad[17]         zero, to the 46-byte Ethernet minimum
 *
 * Captured routing message (27 bytes, off 2..28):
 *   0b 02 00 00 aa 00 04 00 02 04 02 da 05 40 00 0f 00 00 08 00 00 00 00 00 00 00 00
 */
static const uint8_t kRouterHelloVector[46] = {
    0x1b, 0x00, 0x0b, 0x02, 0x00, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,
    0x02, 0xda, 0x05, 0x40, 0x00, 0x0f, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* The 27-byte routing message alone (no length prefix, no pad) -- what OVMX's
 * encoder must reproduce byte-for-byte for a lone L1 router at 1.2. */
static const uint8_t kOracleRoutingMsg[27] = {
    0x0b, 0x02, 0x00, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x02, 0xda,
    0x05, 0x40, 0x00, 0x0f, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00,
};

int main(void)
{
    printf("test_dnet_router_hello: DECnet Phase IV router-hello codec self round-trip\n");

    /* --- decode the synthetic n = 0 vector --- */
    struct dnet_router_hello m;
    size_t consumed = 0;
    int rc = dnet_router_hello_decode(kRouterHelloVector, sizeof(kRouterHelloVector),
                                      &m, &consumed);
    check(rc == DNET_ROUTER_HELLO_OK, "dnet_router_hello_decode(synthetic vector) succeeds");
    check(consumed == 29, "routing message consumed 29 bytes (2 len + 27 msg)");

    check(m.rflags == DNET_RFLAG_ROUTER_HELLO, "rflags == 0x0b (control, router hello)");
    check(m.version == 2, "vers 2");
    check(m.eco == 0, "eco 0");
    check(m.user_eco == 0, "ueco 0");

    static const uint8_t expect_id[6] = { 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04 };
    check(memcmp(m.id, expect_id, 6) == 0, "id == aa:00:04:00:02:04 (captured src)");
    uint16_t src = dnet_addr_from_id(m.id);
    check(dnet_area_of(src) == 1 && dnet_node_of(src) == 2, "src node == 1.2 (area 1, node 2)");

    check(dnet_router_hello_nodetype(&m) == DNET_NODETYPE_L1ROUTER,
          "iinfo node type == L1 router");
    check(m.blksize == 1498, "blksize == 1498");
    check(m.priority == 64, "priority == 64");
    check(m.area == 0, "area == 0");
    check(m.timer == 15, "hello timer == 15 s");
    check(m.mpd == 0, "mpd == 0");

    /* RSLIST tail for n = 0. */
    check(m.rslist_len == 8, "RSLIST-length byte == 8 (n=0: 8 + 0 router-list bytes)");
    static const uint8_t zero7[DNET_ROUTER_HELLO_NAME_LEN] = { 0 };
    check(memcmp(m.name, zero7, sizeof zero7) == 0, "Name field is 7 reserved zero bytes");
    check(m.rslist_count == 0, "RSLIST-count == 0 (no other routers reported)");

    /* --- re-encode: must be byte-identical to the synthetic vector --- */
    uint8_t out[64];
    memset(out, 0x5a, sizeof(out)); /* poison so pad-zeroing is verifiable */
    size_t outlen = 0;
    rc = dnet_router_hello_encode(&m, out, sizeof(out), &outlen);
    check(rc == DNET_ROUTER_HELLO_OK, "dnet_router_hello_encode succeeds");
    check(outlen == sizeof(kRouterHelloVector), "re-encoded length == 46 (padded to Ethernet minimum)");
    check(out[0] == 0x1b && out[1] == 0x00, "emitted DATA LENGTH prefix == 0x001b (27-byte routing message)");
    check(memcmp(out, kRouterHelloVector, sizeof(kRouterHelloVector)) == 0,
          "re-encoded bytes == captured VMS specimen (byte-identical oracle round-trip)");
    check(memcmp(out + 2, kOracleRoutingMsg, sizeof kOracleRoutingMsg) == 0,
          "emitted 27-byte routing message == real VMS wire bytes exactly");

    /* --- ORACLE EXACT-MATCH from a FRESH build (the engine's emit path): a lone
     * L1 router at 1.2 / blksize 1498 / priority 64 / timer 15 emits EXACTLY the
     * real captured VMS routing message, not merely a decode->re-encode. --- */
    struct dnet_router_hello lone;
    memset(&lone, 0, sizeof lone);
    lone.rflags = DNET_RFLAG_ROUTER_HELLO;
    lone.version = 2;
    memcpy(lone.id, expect_id, 6);          /* aa:00:04:00:02:04 = node 1.2 */
    lone.iinfo = DNET_NODETYPE_L1ROUTER;
    lone.blksize = 1498;
    lone.priority = 64;
    lone.area = 0;
    lone.timer = 15;
    lone.mpd = 0;
    lone.rslist_count = 0;                    /* no other routers -> the n=0 tail */
    uint8_t lonebuf[64];
    size_t lonelen = 0;
    rc = dnet_router_hello_encode(&lone, lonebuf, sizeof lonebuf, &lonelen);
    check(rc == DNET_ROUTER_HELLO_OK, "fresh lone-router encode succeeds");
    check(lonebuf[0] == 0x1b && lonebuf[1] == 0x00, "fresh build DATA LENGTH == 0x1b (27)");
    check(memcmp(lonebuf + 2, kOracleRoutingMsg, sizeof kOracleRoutingMsg) == 0,
          "fresh lone-L1-router build == real VMS captured 27-byte router hello, byte-for-byte");

    /* --- n = 1: a message carrying one 7-byte router-list entry round-trips --- */
    struct dnet_router_hello r1, d1;
    memset(&r1, 0, sizeof r1);
    r1.rflags = DNET_RFLAG_ROUTER_HELLO;
    r1.version = 2;
    memcpy(r1.id, expect_id, 6);
    r1.iinfo = DNET_NODETYPE_L1ROUTER;
    r1.blksize = 1498;
    r1.priority = 64;
    r1.timer = 15;
    r1.rslist_count = DNET_ROUTER_HELLO_RSENTRY;   /* one 7-byte entry */
    static const uint8_t ent[DNET_ROUTER_HELLO_RSENTRY] = {
        0xaa, 0x00, 0x04, 0x00, 0x0a, 0x08, 0x00
    };
    memcpy(r1.rslist, ent, sizeof ent);

    uint8_t w1[64];
    size_t w1len = 0;
    rc = dnet_router_hello_encode(&r1, w1, sizeof w1, &w1len);
    check(rc == DNET_ROUTER_HELLO_OK, "encode(n=1) succeeds");
    check(w1[0] == 0x22 && w1[1] == 0x00, "n=1 DATA LENGTH prefix == 0x0022 (34-byte routing message)");
    check(w1[2 + 18] == (DNET_ROUTER_HELLO_RSLIST_HDR + DNET_ROUTER_HELLO_RSENTRY),
          "n=1 RSLIST-length byte == 15 (8 + 7)");
    check(w1[2 + 26] == DNET_ROUTER_HELLO_RSENTRY, "n=1 RSLIST-count byte == 7");

    consumed = 0;
    rc = dnet_router_hello_decode(w1, w1len, &d1, &consumed);
    check(rc == DNET_ROUTER_HELLO_OK, "decode(n=1) succeeds");
    check(d1.rslist_count == DNET_ROUTER_HELLO_RSENTRY, "n=1 rslist_count round-trips (7)");
    check(d1.rslist_len == (DNET_ROUTER_HELLO_RSLIST_HDR + DNET_ROUTER_HELLO_RSENTRY),
          "n=1 rslist_len round-trips (15)");
    check(memcmp(d1.rslist, ent, sizeof ent) == 0, "n=1 router-list entry bytes round-trip");
    check(d1.priority == 64 && d1.timer == 15 && d1.blksize == 1498 &&
          memcmp(d1.id, expect_id, 6) == 0,
          "n=1 fixed fields round-trip alongside the entry");

    /* --- a too-small output buffer is rejected, not overrun --- */
    uint8_t tiny[8];
    rc = dnet_router_hello_encode(&m, tiny, sizeof(tiny), &outlen);
    check(rc == DNET_ROUTER_HELLO_ENOSPACE, "encode into an 8-byte buffer returns ENOSPACE");

    /* --- an oversized router-list is rejected, not overrun --- */
    struct dnet_router_hello big;
    memset(&big, 0, sizeof big);
    big.rflags = DNET_RFLAG_ROUTER_HELLO;
    big.rslist_count = DNET_ROUTER_HELLO_MAX_RSLIST + 1; /* clamps caller error */
    rc = dnet_router_hello_encode(&big, out, sizeof(out), &outlen);
    check(rc == DNET_ROUTER_HELLO_EBADLEN, "encode with rslist_count past the cap returns EBADLEN");

    /* --- a truncated input is rejected, not read past the end --- */
    rc = dnet_router_hello_decode(kRouterHelloVector, 10, &m, &consumed);
    check(rc == DNET_ROUTER_HELLO_ETRUNC, "decode of a 10-byte truncation returns ETRUNC");

    /* --- a DATA LENGTH shorter than the fixed message is rejected --- */
    uint8_t badlen[8] = { 0x05, 0x00, 0, 0, 0, 0, 0, 0 }; /* msglen=5 < 27 fixed */
    rc = dnet_router_hello_decode(badlen, sizeof(badlen), &m, &consumed);
    check(rc == DNET_ROUTER_HELLO_EBADLEN, "decode with DATA LENGTH < fixed size returns EBADLEN");

    /* --- null arguments are rejected --- */
    rc = dnet_router_hello_decode(NULL, 10, &m, &consumed);
    check(rc == DNET_ROUTER_HELLO_EINVAL, "decode(NULL buf) returns EINVAL");
    rc = dnet_router_hello_encode(NULL, out, sizeof(out), &outlen);
    check(rc == DNET_ROUTER_HELLO_EINVAL, "encode(NULL msg) returns EINVAL");

    if (failures == 0) {
        printf("test_dnet_router_hello: ALL CHECKS PASSED\n");
        return 0;
    }
    printf("test_dnet_router_hello: %d CHECK(S) FAILED\n", failures);
    return 1;
}
