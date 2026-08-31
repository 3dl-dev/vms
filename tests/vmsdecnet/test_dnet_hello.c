/*
 * test_dnet_hello.c - oracle round-trip for the DECnet Phase IV Ethernet
 *                     Endnode Hello codec (rd vms-851, rung 1).
 *
 * Ground truth = the lab-oracle wire specimen captured under rd vms-3be
 * (PR #665) and committed as a hex dump in docs/decnet-provenance-register.md
 * sec 4.6, "Hex dump, specimen #1 (HELLO)": a 46-byte Ethernet payload sent by
 * lab node VAX1 (OpenVMS VAX V7.3), aa:00:04:00:01:04 (node 1.1) ->
 * ab:00:00:03:00:00, tcpdump-decoded as:
 *   "endnode-hello endnode vers 2 eco 0 ueco 0 src 1.1 blksize 1498
 *    rtr 0.0 hello 15 data 2".
 *
 * The test (a) DECODES the captured bytes and asserts every field value the
 * provenance register's decode names, and (b) re-ENCODES the decoded message
 * and asserts it is byte-identical to the 46 captured bytes. This is the
 * Rule-8 oracle proof for this rung -- the codec is validated against real
 * VAX wire, not against itself.
 *
 * Specimen #2 in the register is recorded as an "identical decode to #1" but
 * its raw bytes were not committed (pcaps are cited-not-committed, Rule 8
 * practice), so it is not reproduced here -- fabricating its bytes would
 * violate the no-invented-wire-data rule. Specimen #1 is the committed
 * ground-truth vector.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dnet_hello.h"

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
 * docs/decnet-provenance-register.md sec 4.6, specimen #1 (HELLO), verbatim:
 *   0x0000:  2200 0d02 0000 aa00 0400 0104 03da 0500
 *   0x0010:  0000 0000 0000 0000 aa00 0400 0000 0f00
 *   0x0020:  0002 aaaa 0000 0000 0000 0000 0000
 */
static const uint8_t kHelloSpecimen1[46] = {
    0x22, 0x00, 0x0d, 0x02, 0x00, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x03, 0xda, 0x05, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0f, 0x00,
    0x00, 0x02, 0xaa, 0xaa, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

int main(void)
{
    printf("test_dnet_hello: DECnet Phase IV endnode-hello codec vs vms-3be oracle\n");

    /* --- decode the captured specimen --- */
    struct dnet_endnode_hello m;
    size_t consumed = 0;
    int rc = dnet_hello_decode(kHelloSpecimen1, sizeof(kHelloSpecimen1), &m, &consumed);
    check(rc == DNET_HELLO_OK, "dnet_hello_decode(specimen #1) succeeds");
    check(consumed == 36, "routing message consumed 36 bytes (2 len + 34 msg)");

    /* --- field values named by the provenance-register decode --- */
    check(m.rflags == DNET_RFLAG_ENDNODE_HELLO, "rflags == 0x0d (control, endnode hello)");
    check(m.version == 2, "vers 2");
    check(m.eco == 0, "eco 0");
    check(m.user_eco == 0, "ueco 0");

    static const uint8_t expect_id[6] = { 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04 };
    check(memcmp(m.id, expect_id, 6) == 0, "id == aa:00:04:00:01:04");
    uint16_t src = dnet_addr_from_id(m.id);
    check(dnet_area_of(src) == 1 && dnet_node_of(src) == 1, "src node == 1.1 (area 1, node 1)");

    check(dnet_hello_nodetype(&m) == DNET_NODETYPE_ENDNODE, "iinfo node type == endnode");
    check(m.blksize == 1498, "blksize == 1498");
    check(m.area == 0, "area == 0");

    static const uint8_t zero8[8] = { 0 };
    check(memcmp(m.seed, zero8, 8) == 0, "seed == all zero");

    static const uint8_t expect_rtr[6] = { 0xaa, 0x00, 0x04, 0x00, 0x00, 0x00 };
    check(memcmp(m.neighbor, expect_rtr, 6) == 0, "neighbor == aa:00:04:00:00:00");
    uint16_t rtr = dnet_addr_from_id(m.neighbor);
    check(dnet_area_of(rtr) == 0 && dnet_node_of(rtr) == 0, "rtr (designated router) == 0.0");

    check(m.timer == 15, "hello timer == 15 s");
    check(m.mpd == 0, "mpd == 0");
    check(m.datalen == 2, "test data length == 2");
    static const uint8_t expect_data[2] = { 0xaa, 0xaa };
    check(memcmp(m.data, expect_data, 2) == 0, "test data == aa aa");

    /* --- re-encode: must be byte-identical to the captured 46 bytes --- */
    uint8_t out[64];
    memset(out, 0x5a, sizeof(out)); /* poison so pad-zeroing is verifiable */
    size_t outlen = 0;
    rc = dnet_hello_encode(&m, out, sizeof(out), &outlen);
    check(rc == DNET_HELLO_OK, "dnet_hello_encode succeeds");
    check(outlen == sizeof(kHelloSpecimen1), "re-encoded length == 46 (padded to Ethernet minimum)");
    check(memcmp(out, kHelloSpecimen1, sizeof(kHelloSpecimen1)) == 0,
          "re-encoded bytes == captured specimen (byte-identical round-trip)");

    /* --- a too-small output buffer is rejected, not overrun --- */
    uint8_t tiny[8];
    rc = dnet_hello_encode(&m, tiny, sizeof(tiny), &outlen);
    check(rc == DNET_HELLO_ENOSPACE, "encode into an 8-byte buffer returns ENOSPACE");

    /* --- a truncated input is rejected, not read past the end --- */
    rc = dnet_hello_decode(kHelloSpecimen1, 20, &m, &consumed);
    check(rc == DNET_HELLO_ETRUNC, "decode of a 20-byte truncation returns ETRUNC");

    if (failures == 0) {
        printf("test_dnet_hello: ALL CHECKS PASSED\n");
        return 0;
    }
    printf("test_dnet_hello: %d CHECK(S) FAILED\n", failures);
    return 1;
}
