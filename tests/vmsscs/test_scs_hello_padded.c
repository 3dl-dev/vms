/*
 * test_scs_hello_padded.c - unit test for the padded directed-HELLO builder
 * (vms-9f3, NISCA channel packet-size verification, spec sec 4k).
 *
 * The padded HELLO is a genuine 120-byte-SCA directed HELLO (sec 4b) whose
 * SCA content is zero-extended up to NISCS_MAX_PKTSZ, with ONLY the SCA length
 * field at abs 14-15 changed. This test proves exactly that GROUNDED shape:
 *   - the frame is 14 + N bytes on the wire for a requested total SCA len N;
 *   - the header (abs 16..133) is BYTE-IDENTICAL to the unpadded directed HELLO;
 *   - the SCA length field (abs 14-15) encodes N-2 (LE), matching the wire
 *     ladder 1500/1069/853 -> 0x05da/0x042b/0x0353;
 *   - the pad tail (abs 134 .. 14+N) is pure zeros;
 *   - smaller probes are byte-identical PREFIXES of the 1500-byte frame apart
 *     from the length field (spec sec 4k retransmit/prefix proof).
 * This is the REQUIRED targeted unit test for vms-9f3; it does NOT replace the
 * live-wire capture+SDA-oracle admission proof (the item's DONE condition).
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

/* Returns 1 iff buf[off..off+len) are all zero. */
static int all_zero(const uint8_t *buf, size_t off, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[off + i] != 0) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    printf("test_scs_hello_padded: padded directed-HELLO builder (vms-9f3)\n");

    struct scs_hello_params p;
    memset(&p, 0, sizeof(p));
    static const uint8_t peer_mac[6]  = { 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04 }; /* VAX1 logical */
    static const uint8_t our_hw_mac[6] = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 }; /* OVMX HW MAC */
    static const uint8_t our_logical[6] = { 0xaa, 0x00, 0x04, 0x00, 0x06, 0x04 }; /* cluster-logical (vms-9f3) */
    static const uint8_t nonce[4]      = { 0xee, 0x05, 0x39, 0x5b };             /* lab nonce */
    memcpy(p.src_mac, our_hw_mac, 6);
    memcpy(p.src_logical, our_logical, 6);
    strncpy(p.node_name, "OVMX", sizeof(p.node_name) - 1);
    p.timer_tick = 0x11223344;
    const uint16_t incarnation = 1;

    /* Reference: the plain 134-byte directed HELLO with the same identity. */
    uint8_t unpadded[SCS_HELLO_FRAME_LEN];
    check(scs_hello_build_directed_frame(&p, peer_mac, nonce, incarnation, unpadded) == 0,
          "scs_hello_build_directed_frame (reference) succeeds");

    /* --- N = 1500 (NISCS_MAX_PKTSZ + 2): the max probe --- */
    static uint8_t pad1500[SCS_HELLO_PADDED_MAX_FRAME];
    memset(pad1500, 0xAA, sizeof(pad1500)); /* poison so the zero pad is verifiable */
    size_t len1500 = 0;
    check(scs_hello_build_padded_directed_frame(&p, peer_mac, nonce, incarnation,
                                                SCS_HELLO_PADDED_MAX_SCA,
                                                pad1500, sizeof(pad1500), &len1500) == 0,
          "build padded HELLO N=1500 succeeds");
    check(len1500 == 14u + SCS_HELLO_PADDED_MAX_SCA, "N=1500 wire length == 1514 bytes");

    /* Ethernet header + SCA body from abs 16 on are byte-identical to the
     * unpadded directed HELLO (the ONLY header difference is the length field). */
    check(memcmp(pad1500 + 0, unpadded + 0, 14) == 0,
          "abs 0-13 (Ethernet header) identical to unpadded directed HELLO");
    check(memcmp(pad1500 + 16, unpadded + 16, SCS_HELLO_FRAME_LEN - 16) == 0,
          "abs 16-133 (SCA body) byte-identical to unpadded directed HELLO");

    /* Length field encodes N-2 (LE): 1500-2 = 1498 = 0x05da. */
    check(pad1500[14] == 0xda && pad1500[15] == 0x05,
          "SCA length field == 0x05da (1498 -> total 1500, GROUNDED sec 4k)");
    /* Unpadded reference still carries the plain 120-byte length 0x0076. */
    check(unpadded[14] == 0x76 && unpadded[15] == 0x00,
          "unpadded reference length field == 0x0076 (total 120)");

    /* HELLO message-class byte + directed per-frame word survive (sec 4a/4k). */
    check(pad1500[36] == 0x05, "message-class byte (abs 36) == 0x05 (HELLO)");
    check(pad1500[30] == 0xb3, "per-frame word (abs 30) == 0xb3 (directed)");
    check(pad1500[92] == (uint8_t)(incarnation & 0xff) &&
          pad1500[93] == (uint8_t)((incarnation >> 8) & 0xff),
          "incarnation echo (abs 92-93) preserved from directed builder");

    /* The pad tail (abs 134 .. 14+1500) is PURE ZERO (1380 bytes, GROUNDED). */
    check(all_zero(pad1500, SCS_HELLO_FRAME_LEN, len1500 - SCS_HELLO_FRAME_LEN),
          "pad tail abs 134..1514 is pure zero (1380 bytes)");

    /* --- N = 853: a mid-ladder probe; must be a byte-identical PREFIX of the
     * 1500-byte frame apart from the length field (spec sec 4k). --- */
    static uint8_t pad853[SCS_HELLO_PADDED_MAX_FRAME];
    memset(pad853, 0xAA, sizeof(pad853));
    size_t len853 = 0;
    check(scs_hello_build_padded_directed_frame(&p, peer_mac, nonce, incarnation,
                                                853, pad853, sizeof(pad853), &len853) == 0,
          "build padded HELLO N=853 succeeds");
    check(len853 == 14u + 853u, "N=853 wire length == 867 bytes");
    check(pad853[14] == 0x53 && pad853[15] == 0x03,
          "N=853 SCA length field == 0x0353 (851 -> total 853, GROUNDED sec 4k)");
    /* Prefix identity: everything from abs 16 up to the end of the 853 frame
     * matches the 1500-byte frame (both share the same directed HELLO + zeros). */
    check(memcmp(pad853 + 16, pad1500 + 16, len853 - 16) == 0,
          "N=853 is a byte-identical prefix of N=1500 apart from the length field");
    check(all_zero(pad853, SCS_HELLO_FRAME_LEN, len853 - SCS_HELLO_FRAME_LEN),
          "N=853 pad tail is pure zero");

    /* --- N = 120: degenerate case == a plain directed HELLO --- */
    static uint8_t pad120[SCS_HELLO_PADDED_MAX_FRAME];
    size_t len120 = 0;
    check(scs_hello_build_padded_directed_frame(&p, peer_mac, nonce, incarnation,
                                                SCS_HELLO_SCA_LEN,
                                                pad120, sizeof(pad120), &len120) == 0,
          "build padded HELLO N=120 (degenerate) succeeds");
    check(len120 == SCS_HELLO_FRAME_LEN &&
          memcmp(pad120, unpadded, SCS_HELLO_FRAME_LEN) == 0,
          "N=120 == plain 134-byte directed HELLO (byte-identical)");

    /* --- vms-9f3 REQUIRED: the timer field (abs 96-101) is a LIVE 48-bit LE
     * value, not static. Two builds with different tick inputs must differ at
     * abs 96 (and across the whole 6-byte field), while every identity field
     * (eth-src, abs-24 src-logical, abs-92 incarnation, join nonce, recv-side
     * fields) stays byte-identical -- proving only the timer advances. --- */
    struct scs_hello_params pa = p, pb = p;
    pa.timer_tick = 0x0000111111111111ull;
    pb.timer_tick = 0x0000222222222222ull;
    static uint8_t ba[SCS_HELLO_PADDED_MAX_FRAME];
    static uint8_t bb[SCS_HELLO_PADDED_MAX_FRAME];
    size_t la = 0, lb = 0;
    check(scs_hello_build_padded_directed_frame(&pa, peer_mac, nonce, incarnation,
                                                SCS_HELLO_PADDED_MAX_SCA, ba, sizeof(ba), &la) == 0 &&
          scs_hello_build_padded_directed_frame(&pb, peer_mac, nonce, incarnation,
                                                SCS_HELLO_PADDED_MAX_SCA, bb, sizeof(bb), &lb) == 0,
          "two padded HELLOs with different tick inputs build");
    check(ba[96] != bb[96], "timer abs 96 ADVANCES with tick input (non-static, vms-9f3)");
    check(memcmp(ba + 96, bb + 96, 6) != 0, "timer field abs 96-101 differs between the two builds");
    check(memcmp(ba + 6, bb + 6, 6) == 0, "eth-src (abs 6) UNCHANGED across the timer change");
    check(memcmp(ba + 24, bb + 24, 6) == 0, "src-logical (abs 24) UNCHANGED across the timer change");
    check(memcmp(ba + 92, bb + 92, 2) == 0, "incarnation echo (abs 92-93) UNCHANGED across the timer change");
    check(memcmp(ba + 68, bb + 68, 4) == 0, "join nonce (abs 68-71) UNCHANGED across the timer change");
    /* The whole frame apart from the 6 timer bytes is identical (abs 16-95 and
     * abs 102..end), so the timer is the SOLE per-frame variation (spec sec 4k). */
    check(memcmp(ba + 16, bb + 16, 96 - 16) == 0 &&
          memcmp(ba + 102, bb + 102, la - 102) == 0,
          "ONLY abs 96-101 varies between the two builds (rest byte-identical)");

    /* The full 48-bit value lands LE across abs 96-101: reproduce the golden
     * af2 joiner tick (0x021561fad2c0 -> c0 d2 fa 61 15 02), proving abs 100-101
     * are LIVE timer bytes now, not the old frozen 0x0099 (vms-9f3). */
    struct scs_hello_params pg = p;
    pg.timer_tick = 0x021561fad2c0ull;
    static uint8_t bg[SCS_HELLO_PADDED_MAX_FRAME];
    size_t lg = 0;
    check(scs_hello_build_padded_directed_frame(&pg, peer_mac, nonce, incarnation,
                                                SCS_HELLO_PADDED_MAX_SCA, bg, sizeof(bg), &lg) == 0,
          "build padded HELLO with golden-magnitude tick");
    static const uint8_t golden_timer[6] = { 0xc0, 0xd2, 0xfa, 0x61, 0x15, 0x02 };
    check(memcmp(bg + 96, golden_timer, 6) == 0,
          "abs 96-101 == golden af2 joiner timer (LE48), abs 100-101 live not 0x0099 (vms-9f3)");

    /* --- error paths --- */
    uint8_t small[64];
    size_t dummy = 0;
    check(scs_hello_build_padded_directed_frame(NULL, peer_mac, nonce, 1,
                                                1500, pad1500, sizeof(pad1500), &dummy) == -1,
          "NULL params rejected");
    check(scs_hello_build_padded_directed_frame(&p, peer_mac, nonce, 1,
                                                1500, NULL, sizeof(pad1500), &dummy) == -1,
          "NULL output buffer rejected");
    check(scs_hello_build_padded_directed_frame(&p, peer_mac, nonce, 1,
                                                1500, pad1500, sizeof(pad1500), NULL) == -1,
          "NULL frame_len_out rejected");
    check(scs_hello_build_padded_directed_frame(&p, peer_mac, nonce, 1,
                                                119, pad1500, sizeof(pad1500), &dummy) == -1,
          "total_sca_len < 120 rejected");
    check(scs_hello_build_padded_directed_frame(&p, peer_mac, nonce, 1,
                                                (uint16_t)(SCS_HELLO_PADDED_MAX_SCA + 1),
                                                pad1500, sizeof(pad1500), &dummy) == -1,
          "total_sca_len > NISCS_MAX_PKTSZ+2 rejected");
    check(scs_hello_build_padded_directed_frame(&p, peer_mac, nonce, 1,
                                                1500, small, sizeof(small), &dummy) == -1,
          "out_cap too small rejected");

    printf("test_scs_hello_padded: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
