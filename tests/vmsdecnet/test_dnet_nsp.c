/*
 * test_dnet_nsp.c - oracle round-trip for the DECnet Phase IV NSP transport
 *                   codec (rd vms-6986, rung 2).
 *
 * Ground truth = the lab-oracle wire specimen captured under rd vms-3be
 * (PR #665) and committed as a hex dump in docs/decnet-provenance-register.md
 * sec 4.6, "Hex dump, specimen #3 (NSP Connect Initiate)": a 53-byte routing
 * frame sent by lab node VAX1 (OpenVMS VAX V7.3, aa:00:04:00:01:04, node 1.1)
 * to VAX2 (aa:00:04:00:02:04, node 1.2), tcpdump-decoded as
 *   "1.1 > 1.2 51 conn-initiate 8193>0 ver 4.1 segsize 1459"
 * with the plaintext access-control string "SYSTEM" in its connect data.
 *
 * The NSP codec owns the NSP transport PDU only; the frame's 2-byte data-link
 * length prefix, 1-byte routing pad, and 21-byte Phase IV long-data-packet
 * routing header (the aa:00:04:00:0x:04 D-ID/S-ID addresses) belong to the
 * routing rung (dnet_hello.* family), not here. Those 24 bytes are asserted
 * for provenance below, then the NSP PDU (frame offset 0x18) is handed to
 * dnet_nsp_decode. The committed 53 bytes are reproduced verbatim (verifiable
 * against the register), and the re-encoded NSP PDU is byte-identical to
 * frame[0x18..] -- the Rule-8 oracle proof for this rung.
 *
 * ORACLE COVERAGE. Only ONE NSP specimen was committed (Connect Initiate); the
 * captured handshake never completed (VAX2's DECnet permanent DB unconfigured,
 * register sec 4.6). So Connect Confirm / Data / Data-Ack / Disconnect are
 * SPEC-DERIVED (public DNA Phase IV NSP spec) and are proven here only by codec
 * self round-trip -- no specimen bytes are fabricated for them.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dnet_nsp.h"

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
 * docs/decnet-provenance-register.md sec 4.6, specimen #3 (NSP Connect
 * Initiate), verbatim:
 *   0x0000:  3300 812e 0000 aa00 0400 0204 0000 aa00
 *   0x0010:  0400 0104 0000 0000 1800 0001 2001 03b3
 *   0x0020:  0500 2a02 001a 0220 2006 5359 5354 454d
 *   0x0030:  2700 0000 00
 */
static const uint8_t kNspSpecimen3Frame[53] = {
    0x33, 0x00, 0x81, 0x2e, 0x00, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x00, 0x00, 0xaa, 0x00,
    0x04, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x01, 0x20, 0x01, 0x03, 0xb3,
    0x05, 0x00, 0x2a, 0x02, 0x00, 0x1a, 0x02, 0x20, 0x20, 0x06, 0x53, 0x59, 0x53, 0x54, 0x45, 0x4d,
    0x27, 0x00, 0x00, 0x00, 0x00,
};

/* Where the NSP PDU begins in the routing frame: 2 (length prefix) + 1 (routing
 * pad byte 0x81) + 21 (long-data-packet routing header) = 24 = 0x18. */
#define NSP_PDU_OFF   0x18
#define NSP_PDU_LEN   (sizeof(kNspSpecimen3Frame) - NSP_PDU_OFF)  /* 29 */

static void test_oracle_ci(void)
{
    printf("[oracle] NSP Connect Initiate vs vms-3be specimen #3\n");

    /* --- provenance: the routing prefix this codec does NOT own --- */
    check(kNspSpecimen3Frame[0] == 0x33 && kNspSpecimen3Frame[1] == 0x00,
          "frame data-link length prefix == 0x0033 = 51 (routing message bytes)");
    check(kNspSpecimen3Frame[2] == 0x81,
          "routing pad byte 0x81 (high bit set, 1 byte) precedes the routing header");
    /* long-data-packet routing header carries the two DECnet Ethernet ids */
    static const uint8_t dst_id[6] = { 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04 }; /* node 1.2 */
    static const uint8_t src_id[6] = { 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04 }; /* node 1.1 */
    check(memcmp(kNspSpecimen3Frame + 6, dst_id, 6) == 0,
          "routing D-ID == aa:00:04:00:02:04 (dest node 1.2)");
    check(memcmp(kNspSpecimen3Frame + 14, src_id, 6) == 0,
          "routing S-ID == aa:00:04:00:01:04 (src node 1.1)");
    check(NSP_PDU_LEN == 29, "NSP PDU is 29 bytes at frame offset 0x18");

    /* --- decode the NSP PDU --- */
    struct dnet_nsp_msg m;
    size_t consumed = 0;
    int rc = dnet_nsp_decode(kNspSpecimen3Frame + NSP_PDU_OFF, NSP_PDU_LEN, &m, &consumed);
    check(rc == DNET_NSP_OK, "dnet_nsp_decode(specimen #3 NSP PDU) succeeds");
    check(consumed == NSP_PDU_LEN, "decode consumed the whole 29-byte PDU");

    /* --- field values named by the provenance-register decode --- */
    check(m.type == DNET_NSP_T_CI, "type == connect initiate");
    check(m.msgflg == DNET_NSP_MSGFLG_CI, "MSGFLG == 0x18 (connect initiate)");
    check(m.dstaddr == 0, "DSTADDR == 0 (fresh connect, dest logical link unassigned)");
    check(m.srcaddr == 8193, "SRCADDR == 8193 (0x2001) -- matches '8193>0'");
    check(m.services == 0x01, "SERVICES == 0x01");
    check(dnet_nsp_version(&m) == DNET_NSP_VER_41, "INFO version == 4.1 -- matches 'ver 4.1'");
    check(m.segsize == 1459, "SEGSIZE == 1459 (0x05b3) -- matches 'segsize 1459'");
    check(m.datalen == 20, "connect data == 20 bytes (opaque session-control payload)");
    /* the plaintext access-control username sits inside the opaque connect data */
    check(m.datalen >= 15 && memcmp(m.data + 9, "SYSTEM", 6) == 0,
          "connect data carries plaintext access-control string 'SYSTEM'");

    /* --- re-encode: must be byte-identical to the captured NSP PDU --- */
    uint8_t out[64];
    memset(out, 0x5a, sizeof(out)); /* poison */
    size_t outlen = 0;
    rc = dnet_nsp_encode(&m, out, sizeof(out), &outlen);
    check(rc == DNET_NSP_OK, "dnet_nsp_encode succeeds");
    check(outlen == NSP_PDU_LEN, "re-encoded length == 29");
    check(memcmp(out, kNspSpecimen3Frame + NSP_PDU_OFF, NSP_PDU_LEN) == 0,
          "re-encoded bytes == captured NSP PDU (byte-identical round-trip)");
}

/* Round-trip a spec-derived message through encode->decode->encode and assert
 * both encodings agree. This proves codec self-consistency; it is NOT an oracle
 * claim (no captured specimen exists for these types). */
static void roundtrip_selfcheck(const struct dnet_nsp_msg *m, const char *label)
{
    uint8_t enc1[128], enc2[128];
    size_t len1 = 0, len2 = 0;
    char buf[96];

    int rc = dnet_nsp_encode(m, enc1, sizeof(enc1), &len1);
    snprintf(buf, sizeof(buf), "%s: encode succeeds", label);
    check(rc == DNET_NSP_OK, buf);

    struct dnet_nsp_msg d;
    size_t consumed = 0;
    rc = dnet_nsp_decode(enc1, len1, &d, &consumed);
    snprintf(buf, sizeof(buf), "%s: decode succeeds", label);
    check(rc == DNET_NSP_OK, buf);
    snprintf(buf, sizeof(buf), "%s: decode consumes all %zu bytes", label, len1);
    check(consumed == len1, buf);
    snprintf(buf, sizeof(buf), "%s: decoded type matches", label);
    check(d.type == m->type, buf);

    rc = dnet_nsp_encode(&d, enc2, sizeof(enc2), &len2);
    snprintf(buf, sizeof(buf), "%s: re-encode succeeds", label);
    check(rc == DNET_NSP_OK, buf);
    snprintf(buf, sizeof(buf), "%s: re-encode byte-identical (self round-trip)", label);
    check(len1 == len2 && memcmp(enc1, enc2, len1) == 0, buf);
}

static void test_spec_derived(void)
{
    printf("[spec-derived, NOT oracle-verified] Connect Confirm / Data / Ack / Disconnect\n");

    /* Connect Confirm: same field shape as CI, dest link now assigned. */
    struct dnet_nsp_msg cc;
    memset(&cc, 0, sizeof(cc));
    cc.type = DNET_NSP_T_CC;
    cc.msgflg = DNET_NSP_MSGFLG_CC;
    cc.dstaddr = 8193;
    cc.srcaddr = 8194;
    cc.services = 0x01;
    cc.info = DNET_NSP_VER_41;
    cc.segsize = 1459;
    cc.datalen = 3;
    memcpy(cc.data, "\x00\x00\x00", 3);
    roundtrip_selfcheck(&cc, "connect-confirm");

    /* Data segment (BOM+EOM) with a piggyback ACK. */
    struct dnet_nsp_msg dseg;
    memset(&dseg, 0, sizeof(dseg));
    dseg.type = DNET_NSP_T_DATA;
    dseg.msgflg = DNET_NSP_MSGFLG_DATA;      /* 0x60 = BOM|EOM */
    dseg.dstaddr = 8194;
    dseg.srcaddr = 8193;
    dseg.has_acknum = 1;
    dseg.acknum = (uint16_t)(DNET_NSP_ACK_QUAL | 5); /* ack of segment 5 */
    dseg.segnum = 1;                          /* segment number, no high bits */
    dseg.datalen = 5;
    memcpy(dseg.data, "hello", 5);
    roundtrip_selfcheck(&dseg, "data-segment+ack");

    /* Data segment without a piggyback ACK. */
    struct dnet_nsp_msg dseg2 = dseg;
    dseg2.has_acknum = 0;
    dseg2.acknum = 0;
    roundtrip_selfcheck(&dseg2, "data-segment");

    /* Data acknowledgement, with an other-data cross-subchannel ack. */
    struct dnet_nsp_msg ack;
    memset(&ack, 0, sizeof(ack));
    ack.type = DNET_NSP_T_ACK;
    ack.msgflg = DNET_NSP_MSGFLG_ACK;
    ack.dstaddr = 8193;
    ack.srcaddr = 8194;
    ack.has_acknum = 1;
    ack.acknum = (uint16_t)(DNET_NSP_ACK_QUAL | 7);
    ack.has_ackoth = 1;
    ack.ackoth = (uint16_t)(DNET_NSP_ACK_QUAL | 2);
    roundtrip_selfcheck(&ack, "data-ack+other");

    /* Data acknowledgement, acknum only. */
    struct dnet_nsp_msg ack2 = ack;
    ack2.has_ackoth = 0;
    ack2.ackoth = 0;
    roundtrip_selfcheck(&ack2, "data-ack");

    /* Disconnect Initiate with a reason code and disconnect data. */
    struct dnet_nsp_msg di;
    memset(&di, 0, sizeof(di));
    di.type = DNET_NSP_T_DI;
    di.msgflg = DNET_NSP_MSGFLG_DI;
    di.dstaddr = 8194;
    di.srcaddr = 8193;
    di.reason = 42;                           /* a disconnect reason code */
    di.datalen = 2;
    memcpy(di.data, "\x00\x00", 2);
    roundtrip_selfcheck(&di, "disconnect-initiate");

    /* Disconnect Initiate with no disconnect data. */
    struct dnet_nsp_msg di2 = di;
    di2.datalen = 0;
    roundtrip_selfcheck(&di2, "disconnect-initiate-bare");

    /* Disconnect Confirm (rd vms-c23): the responder's terminal ack of a DI.
     * SPEC-DERIVED, self round-trip only; classified distinctly from DI. */
    struct dnet_nsp_msg dc;
    memset(&dc, 0, sizeof(dc));
    dc.type = DNET_NSP_T_DC;
    dc.msgflg = DNET_NSP_MSGFLG_DC;
    dc.dstaddr = 8193;
    dc.srcaddr = 8194;
    dc.reason = DNET_NSP_REASON_DISC_COMPLETE;
    roundtrip_selfcheck(&dc, "disconnect-confirm");
    /* And that a DC decodes back as a DC, not confused with a DI. */
    {
        uint8_t b[16];
        size_t bl = 0;
        struct dnet_nsp_msg back;
        check(dnet_nsp_encode(&dc, b, sizeof(b), &bl) == DNET_NSP_OK &&
              b[0] == DNET_NSP_MSGFLG_DC &&
              dnet_nsp_decode(b, bl, &back, NULL) == DNET_NSP_OK &&
              back.type == DNET_NSP_T_DC && back.reason == DNET_NSP_REASON_DISC_COMPLETE,
              "disconnect-confirm classifies as DC (distinct from DI)");
    }
}

static void test_negatives(void)
{
    printf("[negatives] short buffers and malformed input are rejected, not overrun\n");

    /* Re-decode the oracle CI to get a valid message to re-encode. */
    struct dnet_nsp_msg m;
    size_t consumed = 0;
    int rc = dnet_nsp_decode(kNspSpecimen3Frame + NSP_PDU_OFF, NSP_PDU_LEN, &m, &consumed);
    check(rc == DNET_NSP_OK, "setup: decode oracle CI");

    /* ENOSPACE: encode into a buffer too small for the 29-byte PDU. */
    uint8_t tiny[8];
    size_t outlen = 0;
    rc = dnet_nsp_encode(&m, tiny, sizeof(tiny), &outlen);
    check(rc == DNET_NSP_ENOSPACE, "encode into an 8-byte buffer returns ENOSPACE");

    /* ENOSPACE boundary: one byte short is rejected, exact size succeeds. */
    uint8_t exact[NSP_PDU_LEN];
    rc = dnet_nsp_encode(&m, exact, NSP_PDU_LEN - 1, &outlen);
    check(rc == DNET_NSP_ENOSPACE, "encode with cap == PDU_LEN-1 returns ENOSPACE");
    rc = dnet_nsp_encode(&m, exact, NSP_PDU_LEN, &outlen);
    check(rc == DNET_NSP_OK && outlen == NSP_PDU_LEN, "encode with cap == PDU_LEN succeeds");

    /* ETRUNC: a PDU shorter than the 5-byte common header. */
    rc = dnet_nsp_decode(kNspSpecimen3Frame + NSP_PDU_OFF, 4, &m, &consumed);
    check(rc == DNET_NSP_ETRUNC, "decode of a 4-byte truncation returns ETRUNC");

    /* ETRUNC: header present but CI SERVICES/INFO/SEGSIZE truncated. */
    rc = dnet_nsp_decode(kNspSpecimen3Frame + NSP_PDU_OFF, 7, &m, &consumed);
    check(rc == DNET_NSP_ETRUNC, "decode of a CI truncated inside SEGSIZE returns ETRUNC");

    /* EBADTYPE: a reserved MSGFLG class (0x0c) is rejected. */
    uint8_t bad[5] = { 0x0c, 0x00, 0x00, 0x00, 0x00 };
    rc = dnet_nsp_decode(bad, sizeof(bad), &m, &consumed);
    check(rc == DNET_NSP_EBADTYPE, "decode of MSGFLG in reserved class 0x0c returns EBADTYPE");

    /* EINVAL: null arguments. */
    rc = dnet_nsp_decode(NULL, 29, &m, &consumed);
    check(rc == DNET_NSP_EINVAL, "decode(NULL buf) returns EINVAL");
    rc = dnet_nsp_encode(NULL, exact, sizeof(exact), &outlen);
    check(rc == DNET_NSP_EINVAL, "encode(NULL msg) returns EINVAL");
}

int main(void)
{
    printf("test_dnet_nsp: DECnet Phase IV NSP transport codec vs vms-3be oracle\n");

    test_oracle_ci();
    test_spec_derived();
    test_negatives();

    if (failures == 0) {
        printf("test_dnet_nsp: ALL CHECKS PASSED\n");
        return 0;
    }
    printf("test_dnet_nsp: %d CHECK(S) FAILED\n", failures);
    return 1;
}
