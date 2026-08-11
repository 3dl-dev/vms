/*
 * test_scs_hello.c - unit test for the spec-valid multicast HELLO builder
 * (vms-b62).
 *
 * Builds a HELLO with a known SCSNODE + MAC and asserts every byte of the
 * 134-byte frame against the layout documented in
 * docs/cluster-protocol-spec.md sec 4(a)/4(b), using the exact constant
 * values read off the wire in scs-idle-baseline.pcap frames 1 and 4 (see
 * scs_hello.h / scs_hello.c for the field-by-field provenance comments).
 * This is the REQUIRED targeted unit test for vms-b62; it does NOT
 * replace the live-wire capture+dissect+diff proof (see the item's DONE
 * condition), nor is it replaced by it.
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

static void check_bytes(const uint8_t *got, const uint8_t *want, size_t n, const char *what)
{
    check(memcmp(got, want, n) == 0, what);
}

int main(void)
{
    printf("test_scs_hello: HELLO frame byte-layout (vms-b62)\n");

    /* --- multicast address, group 1 -- GROUNDED (spec sec 3, sec 4a) --- */
    uint8_t mcast[6];
    scs_hello_multicast_addr(SCS_HELLO_MCAST_GROUP1, mcast);
    static const uint8_t expect_mcast[6] = { 0xAB, 0x00, 0x04, 0x01, 0x01, 0x01 };
    check_bytes(mcast, expect_mcast, 6, "scs_hello_multicast_addr(1) == AB-00-04-01-01-01");

    /* --- build a HELLO with a known test identity --- */
    struct scs_hello_params p;
    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, mcast, 6);
    static const uint8_t test_hw_mac[6] = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 }; /* "OVX", locally-admin bit set */
    /* vms-9f3: the cluster-LOGICAL addr aa:00:04:00:<LE16(sysid)> that a real node
     * writes at abs 24 -- deliberately DISTINCT from the raw HW MAC so the test
     * proves abs 24 carries the logical addr while eth-src/tail carry the HW MAC. */
    static const uint8_t test_src_logical[6] = { 0xaa, 0x00, 0x04, 0x00, 0x06, 0x04 }; /* sysid 1030 */
    memcpy(p.src_mac, test_hw_mac, 6);
    memcpy(p.src_logical, test_src_logical, 6);
    strncpy(p.node_name, "OVMX", sizeof(p.node_name) - 1);
    p.timer_tick = 0x11223344;

    uint8_t out[SCS_HELLO_FRAME_LEN];
    memset(out, 0xAA, sizeof(out)); /* poison so memset-to-zero in the builder is verifiable */
    int rc = scs_hello_build_frame(&p, out);
    check(rc == 0, "scs_hello_build_frame succeeds");

    /* Ethernet header (abs 0-13) */
    check_bytes(out + 0, mcast, 6, "Ethernet dst == cluster multicast addr");
    check_bytes(out + 6, test_hw_mac, 6, "Ethernet src == OVMX HW MAC");
    static const uint8_t ethertype[2] = { 0x60, 0x07 };
    check_bytes(out + 12, ethertype, 2, "Ethertype == 0x6007");

    /* SCA discovery header (abs 14-71, spec sec 4a) */
    static const uint8_t len_field[2] = { 0x76, 0x00 };
    check_bytes(out + 14, len_field, 2, "SCA length field == 0x0076 (-> total 120, GROUNDED)");
    check_bytes(out + 16, mcast, 6, "dest/group logical addr == cluster multicast addr (GROUNDED)");
    static const uint8_t connect_flag[2] = { 0x01, 0x00 };
    check_bytes(out + 22, connect_flag, 2, "connect flag == 0x0001 (observed constant)");
    check_bytes(out + 24, test_src_logical, 6, "src-logical addr == cluster-LOGICAL aa:00:04:00:<sysid>, NOT HW MAC (vms-9f3)");
    check(memcmp(out + 24, test_hw_mac, 6) != 0, "src-logical (abs 24) is DISTINCT from the raw HW MAC (vms-9f3)");
    static const uint8_t perframe_word[2] = { 0xa0, 0x00 };
    check_bytes(out + 30, perframe_word, 2, "per-frame word == 0xa000 (multicast, inferred-constant)");
    static const uint8_t const_prefix[4] = { 0x08, 0x00, 0x00, 0x80 };
    check_bytes(out + 32, const_prefix, 4, "constant prefix == 08 00 00 80 (inferred-constant)");
    check(out[36] == 0x05, "message-class byte == 0x05 (HELLO)");
    static const uint8_t const_suffix[3] = { 0x01, 0x00, 0x00 };
    check_bytes(out + 37, const_suffix, 3, "constant suffix == 01 00 00 (inferred-constant)");
    check(out[40] == SCS_HELLO_NODENAME_LEN, "node-name length prefix == 6 (GROUNDED)");
    check_bytes((const uint8_t *)(out + 41), (const uint8_t *)"OVMX  ", 6, "node name == 'OVMX  ' (space-padded)");
    static const uint8_t cap_span[17] = {
        0x00, 0x80, 0x01, 0xff, 0x83, 0x00, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18
    };
    check_bytes(out + 47, cap_span, 17, "capability/version span matches baseline capture (inferred-constant)");
    check(out[64] == 0x03, "constant byte 64 == 0x03 (inferred-constant)");
    static const uint8_t zero3[3] = { 0, 0, 0 };
    check_bytes(out + 65, zero3, 3, "bytes 65-67 == zero (inferred-constant)");
    static const uint8_t zero4[4] = { 0, 0, 0, 0 };
    check_bytes(out + 68, zero4, 4, "join nonce == 0x00000000 (GROUNDED, multicast HELLO)");

    /* HELLO tail (abs 72-133, spec sec 4b) */
    uint8_t zero20[20];
    memset(zero20, 0, sizeof(zero20));
    check_bytes(out + 72, zero20, 20, "bytes 72-91 == zero padding");
    check_bytes(out + 92, zero4, 2, "directed-HELLO flag == 0x0000 (GROUNDED, multicast)");
    static const uint8_t trailer_9205[2] = { 0x92, 0x05 };
    check_bytes(out + 94, trailer_9205, 2, "constant trailer == 0x9205 (inferred-constant)");
    /* abs 96-101: 48-bit LE timer (vms-9f3). timer_tick=0x11223344 -> 6-byte LE
     * = 44 33 22 11 00 00. The high 2 bytes (abs 100-101) belong to THIS live
     * timer, not to the constant tail -- they are no longer the frozen 0x0099. */
    static const uint8_t timer_le[6] = { 0x44, 0x33, 0x22, 0x11, 0x00, 0x00 };
    check_bytes(out + 96, timer_le, 6, "timer/tick (abs 96-101) == caller-supplied 48-bit value, LE (vms-9f3)");
    static const uint8_t tail_const[10] = {
        0xbc, 0x00, 0x03, 0x58, 0x51, 0x41, 0x00, 0x00, 0x00, 0x00
    };
    check_bytes(out + 102, tail_const, 10, "constant tail (abs 102-111) matches baseline capture (inferred-constant)");
    uint8_t zero8[8];
    memset(zero8, 0, sizeof(zero8));
    check_bytes(out + 112, zero8, 8, "bytes 112-119 == zero padding");
    check_bytes(out + 120, test_hw_mac, 6, "sender's real HW LAN MAC == OVMX HW MAC (GROUNDED)");
    static const uint8_t trailer_2600[2] = { 0x26, 0x00 };
    check_bytes(out + 126, trailer_2600, 2, "constant trailer == 0x2600 (inferred-constant)");
    check_bytes(out + 128, zero4, 2, "poller-sweep marker == 0x0000 (GROUNDED, multicast)");
    static const uint8_t const_0064[2] = { 0x64, 0x00 };
    check_bytes(out + 130, const_0064, 2, "constant == 0x0064 (inferred-constant)");
    check_bytes(out + 132, zero4, 2, "trailer == 0x0000 (inferred-constant)");

    /* --- error paths --- */
    check(scs_hello_build_frame(NULL, out) == -1, "NULL params rejected");
    check(scs_hello_build_frame(&p, NULL) == -1, "NULL output buffer rejected");

    struct scs_hello_params p_maxlen = p;
    memset(p_maxlen.node_name, 0, sizeof(p_maxlen.node_name));
    memcpy(p_maxlen.node_name, "OVMXC1", 6);
    check(scs_hello_build_frame(&p_maxlen, out) == 0,
          "6-char (max) node name accepted");

    /* Fill the whole 7-byte node_name array with non-NUL bytes (no
     * terminator within the buffer) to simulate a caller violating the
     * <=6-char contract; strnlen caps at sizeof(node_name)==7, so this
     * must be rejected (7 > SCS_HELLO_NODENAME_LEN). */
    struct scs_hello_params p_toolong = p;
    memset(p_toolong.node_name, 'X', sizeof(p_toolong.node_name));
    check(scs_hello_build_frame(&p_toolong, out) == -1,
          "node name with no NUL terminator in a 7-byte buffer rejected");

    /* --- vms-708 (spec 4(O.30)): PORT-LEVEL clean-leave LAST GASP ---
     * The authentic immediate-removal datagram a real VAX emits on SHUTDOWN.COM
     * (Davis p.7-29), RE'd VAX-vs-VAX off vaxlab-0 (d94-708-leave frame 4899 +
     * d94-708-leave2 frame 5015). It is a plain MULTICAST HELLO differing at
     * EXACTLY two semantic fields: abs-30 a0->b1 and abs-68..71 zero->cluster
     * token. This test rebuilds a normal HELLO and the last gasp from the SAME
     * params and byte-diffs them, asserting the diff is precisely those two
     * fields (mirroring the two-specimen wire analysis). */
    static const uint8_t lg_nonce[4] = { 0xee, 0x05, 0x39, 0x5b }; /* the lab cluster token */
    uint8_t ref[SCS_HELLO_FRAME_LEN];
    uint8_t lg[SCS_HELLO_FRAME_LEN];
    check(scs_hello_build_frame(&p, ref) == 0, "reference multicast HELLO builds");
    check(scs_hello_build_lastgasp_frame(&p, lg_nonce, lg) == 0, "last-gasp frame builds");

    check(lg[30] == SCS_HELLO_PFW_LASTGASP && lg[30] == 0xb1,
          "last gasp abs-30 == 0xb1 (SCS_HELLO_PFW_LASTGASP), vs 0xa0 on a normal HELLO");
    check(ref[30] == 0xa0, "reference HELLO abs-30 == 0xa0 (the field that changed)");
    check(lg[31] == 0x00, "last gasp abs-31 == 0x00 (high byte unchanged)");
    check_bytes(lg + 68, lg_nonce, 4, "last gasp abs-68..71 == cluster token (ee 05 39 5b)");
    check(memcmp(ref + 68, "\0\0\0\0", 4) == 0, "reference HELLO abs-68..71 == 0 (the field that changed)");

    /* The byte-diff between a normal multicast HELLO and the last gasp built from
     * the SAME params must be EXACTLY abs-30 and abs-68..71 -- no other field.
     * (The abs-96 live timer is identical here because timer_tick is fixed.) */
    int diff_ok = 1;
    for (int i = 14; i < SCS_HELLO_FRAME_LEN; i++) {
        int expected_diff = (i == 30) || (i >= 68 && i <= 71);
        if ((ref[i] != lg[i]) != expected_diff) { diff_ok = 0; break; }
    }
    check(diff_ok,
          "last gasp differs from a normal multicast HELLO at EXACTLY abs-30 and"
          " abs-68..71 -- the two RE'd fields, nothing else (2-specimen grounded)");

    check(scs_hello_build_lastgasp_frame(NULL, lg_nonce, lg) == -1, "NULL params rejected");
    check(scs_hello_build_lastgasp_frame(&p, NULL, lg) == -1, "NULL nonce rejected");
    check(scs_hello_build_lastgasp_frame(&p, lg_nonce, NULL) == -1, "NULL output rejected");

    printf("test_scs_hello: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
