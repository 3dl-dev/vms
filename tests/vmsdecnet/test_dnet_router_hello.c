/*
 * test_dnet_router_hello.c - self round-trip test for the DECnet Phase IV
 *                            Ethernet Router Hello codec (rd vms-0aba).
 *
 * Unlike test_dnet_hello.c, there is NO committed lab-oracle wire specimen
 * for a router hello in docs/decnet-provenance-register.md (searched before
 * writing this file: sec 4.6's HELLO captures are all from lab-2's VAX1,
 * which is an ENDNODE, not a router). Per CLAUDE.md Rule 8, fabricating
 * specimen bytes and presenting them as captured wire is forbidden, so this
 * test instead:
 *   (a) hand-builds a SYNTHETIC router-hello byte vector from the
 *       spec-derived layout documented in dnet_router_hello.h (field offsets
 *       independently computed here, not copied from the encoder), DECODES
 *       it, and asserts every field;
 *   (b) re-ENCODES the decoded message and asserts it is byte-identical to
 *       the hand-built vector -- proving decode and encode agree with each
 *       other and with the documented layout, which is the strongest claim
 *       obtainable without an oracle capture (a genuine self round-trip, not
 *       an oracle round-trip).
 * The E-list bytes in the synthetic vector are arbitrary filler (OVMX carries
 * the E-list as opaque/uninterpreted, per dnet_router_hello.h) -- they are
 * NOT a claim about real DECnet router-list wire content.
 *
 * When a real router-hello specimen is captured and committed to the
 * provenance register, promote this test to an oracle round-trip like
 * test_dnet_hello.c.
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
 * Hand-built synthetic router-hello frame, per the dnet_router_hello.h
 * layout (offsets computed independently of dnet_router_hello.c):
 *
 *   off 0   msglen LE       0x0016 (22 = 18 fixed + 4 elist)
 *   off 2   rflags          0x0b   (control, router hello, msg type 5)
 *   off 3   version         0x02
 *   off 4   eco             0x00
 *   off 5   user_eco        0x00
 *   off 6   id              aa:00:04:00:05:04  (node 1.5)
 *   off 12  iinfo           0x02   (L1 router, low 2 bits = node type 2)
 *   off 13  blksize LE      0x05da (1498)
 *   off 15  priority        0x40   (64)
 *   off 16  area            0x00
 *   off 17  timer LE        0x000f (15)
 *   off 19  mpd             0x00
 *   off 20  elist[4]        aa 00 04 02  (arbitrary opaque filler)
 *   off 24  pad[22]         zero, to the 46-byte Ethernet minimum
 */
static const uint8_t kRouterHelloVector[46] = {
    0x16, 0x00, 0x0b, 0x02, 0x00, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x05, 0x04,
    0x02, 0xda, 0x05, 0x40, 0x00, 0x0f, 0x00, 0x00, 0xaa, 0x00, 0x04, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

int main(void)
{
    printf("test_dnet_router_hello: DECnet Phase IV router-hello codec self round-trip\n");

    /* --- decode the synthetic vector --- */
    struct dnet_router_hello m;
    size_t consumed = 0;
    int rc = dnet_router_hello_decode(kRouterHelloVector, sizeof(kRouterHelloVector),
                                      &m, &consumed);
    check(rc == DNET_ROUTER_HELLO_OK, "dnet_router_hello_decode(synthetic vector) succeeds");
    check(consumed == 24, "routing message consumed 24 bytes (2 len + 22 msg)");

    check(m.rflags == DNET_RFLAG_ROUTER_HELLO, "rflags == 0x0b (control, router hello)");
    check(m.version == 2, "vers 2");
    check(m.eco == 0, "eco 0");
    check(m.user_eco == 0, "ueco 0");

    static const uint8_t expect_id[6] = { 0xaa, 0x00, 0x04, 0x00, 0x05, 0x04 };
    check(memcmp(m.id, expect_id, 6) == 0, "id == aa:00:04:00:05:04");
    uint16_t src = dnet_addr_from_id(m.id);
    check(dnet_area_of(src) == 1 && dnet_node_of(src) == 5, "src node == 1.5 (area 1, node 5)");

    check(dnet_router_hello_nodetype(&m) == DNET_NODETYPE_L1ROUTER,
          "iinfo node type == L1 router");
    check(m.blksize == 1498, "blksize == 1498");
    check(m.priority == 64, "priority == 64");
    check(m.area == 0, "area == 0");
    check(m.timer == 15, "hello timer == 15 s");
    check(m.mpd == 0, "mpd == 0");
    check(m.elist_len == 4, "elist length == 4");
    static const uint8_t expect_elist[4] = { 0xaa, 0x00, 0x04, 0x02 };
    check(memcmp(m.elist, expect_elist, 4) == 0, "elist bytes preserved opaquely");

    /* --- re-encode: must be byte-identical to the synthetic vector --- */
    uint8_t out[64];
    memset(out, 0x5a, sizeof(out)); /* poison so pad-zeroing is verifiable */
    size_t outlen = 0;
    rc = dnet_router_hello_encode(&m, out, sizeof(out), &outlen);
    check(rc == DNET_ROUTER_HELLO_OK, "dnet_router_hello_encode succeeds");
    check(outlen == sizeof(kRouterHelloVector), "re-encoded length == 46 (padded to Ethernet minimum)");
    check(memcmp(out, kRouterHelloVector, sizeof(kRouterHelloVector)) == 0,
          "re-encoded bytes == synthetic vector (byte-identical self round-trip)");

    /* --- a too-small output buffer is rejected, not overrun --- */
    uint8_t tiny[8];
    rc = dnet_router_hello_encode(&m, tiny, sizeof(tiny), &outlen);
    check(rc == DNET_ROUTER_HELLO_ENOSPACE, "encode into an 8-byte buffer returns ENOSPACE");

    /* --- a truncated input is rejected, not read past the end --- */
    rc = dnet_router_hello_decode(kRouterHelloVector, 10, &m, &consumed);
    check(rc == DNET_ROUTER_HELLO_ETRUNC, "decode of a 10-byte truncation returns ETRUNC");

    /* --- a DATA LENGTH shorter than the fixed message is rejected --- */
    uint8_t badlen[8] = { 0x05, 0x00, 0, 0, 0, 0, 0, 0 }; /* msglen=5 < 18 fixed */
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
