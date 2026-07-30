/*
 * test_scs_hello_pfw.c - vms-d94: the directed-HELLO abs-30 channel-verify
 * per-frame word (spec sec 4a offset-30).
 *
 * REQUIRED targeted test for vms-d94. Proves:
 *   (1) scs_hello_response_pfw() implements the GROUNDED request/response rule
 *       (b2->b3, b3->b4, b4->b3), byte-exact to the request/response cadence in
 *       BOTH formation captures (formation-clean-2node.pcap VAXA/VAXB and
 *       formation-ci1-joinwindow.pcap VAX1/VAX2).
 *   (2) scs_hello_build_directed_frame() can produce a b4 CONFIRM frame -- not
 *       just the fixed b3 the old vms-5fe builder held -- and toggles the abs-30
 *       word (b2/b3/b4) per the caller-supplied word, with the high byte abs-31
 *       constant 0x00.
 *   (3) changing ONLY the per-frame word changes ONLY abs-30 -- every other byte
 *       of the directed HELLO (identity, nonce, incarnation, timer, markers) is
 *       unchanged (the vms-d94 "keep everything else unchanged" invariant).
 *   (4) driving the responder through the grounded bootstrap (recv b2 -> b3,
 *       recv b3 -> b4) and the ongoing keepalive (recv b4 -> b3) reaches b4 and
 *       toggles b3<->b4, i.e. OVMX no longer holds b3 forever.
 *
 * Pure builder/logic test (no socket, no live VAX); explicit libc headers so it
 * is musl-static clean.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scs_hello.h"

static int failures;

static void check(int cond, const char *msg)
{
    printf("%s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) {
        failures++;
    }
}

int main(void)
{
    /* --- (1) the grounded response rule --- */
    check(scs_hello_response_pfw(SCS_HELLO_PFW_INIT) == SCS_HELLO_PFW_REQUEST,
          "response rule: recv b2 (INIT) -> reply b3 (REQUEST)");
    check(scs_hello_response_pfw(SCS_HELLO_PFW_REQUEST) == SCS_HELLO_PFW_CONFIRM,
          "response rule: recv b3 (REQUEST) -> reply b4 (CONFIRM) [THE vms-d94 FIX]");
    check(scs_hello_response_pfw(SCS_HELLO_PFW_CONFIRM) == SCS_HELLO_PFW_REQUEST,
          "response rule: recv b4 (CONFIRM) -> reply b3 (re-initiate verify)");
    check(scs_hello_response_pfw(0x00) == SCS_HELLO_PFW_REQUEST,
          "response rule: unknown word -> reply b3 (safe default)");
    check(SCS_HELLO_PFW_INIT == 0xb2 && SCS_HELLO_PFW_REQUEST == 0xb3 &&
          SCS_HELLO_PFW_CONFIRM == 0xb4,
          "per-frame-word constants are the GROUNDED wire values b2/b3/b4");

    /* --- identity for the builder --- */
    struct scs_hello_params p;
    memset(&p, 0, sizeof(p));
    static const uint8_t peer_mac[6] = { 0x08, 0x00, 0x2b, 0xfb, 0x72, 0x36 }; /* VAXA */
    static const uint8_t ovmx_mac[6] = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 };
    static const uint8_t ovmx_log[6] = { 0xaa, 0x00, 0x04, 0x00, 0x06, 0x04 };
    static const uint8_t nonce[4]    = SCS_HELLO_LAB_NONCE_BYTES;
    scs_hello_multicast_addr(SCS_HELLO_MCAST_GROUP1, p.dst_mac);
    memcpy(p.src_mac, ovmx_mac, 6);
    memcpy(p.src_logical, ovmx_log, 6);
    strncpy(p.node_name, "OVMX", sizeof(p.node_name) - 1);
    p.timer_tick = 0x0102030405ULL;

    /* --- (2) the builder can emit b2, b3 AND b4 --- */
    uint8_t f_b2[SCS_HELLO_FRAME_LEN];
    uint8_t f_b3[SCS_HELLO_FRAME_LEN];
    uint8_t f_b4[SCS_HELLO_FRAME_LEN];
    check(scs_hello_build_directed_frame(&p, peer_mac, nonce, 1,
                                         SCS_HELLO_PFW_INIT, f_b2) == 0, "build b2 frame");
    check(scs_hello_build_directed_frame(&p, peer_mac, nonce, 1,
                                         SCS_HELLO_PFW_REQUEST, f_b3) == 0, "build b3 frame");
    check(scs_hello_build_directed_frame(&p, peer_mac, nonce, 1,
                                         SCS_HELLO_PFW_CONFIRM, f_b4) == 0, "build b4 frame");
    check(f_b2[30] == 0xb2 && f_b2[31] == 0x00, "b2 frame: abs30==0xb2, abs31==0x00");
    check(f_b3[30] == 0xb3 && f_b3[31] == 0x00, "b3 frame: abs30==0xb3, abs31==0x00");
    check(f_b4[30] == 0xb4 && f_b4[31] == 0x00,
          "b4 frame: abs30==0xb4 -- builder REACHES b4 (was fixed at b3, vms-5fe)");

    /* --- (3) only abs-30 differs between the b3 and b4 frames --- */
    int only_abs30_differs = 1;
    for (int i = 0; i < SCS_HELLO_FRAME_LEN; i++) {
        if (i == 30) {
            continue;
        }
        if (f_b3[i] != f_b4[i]) {
            only_abs30_differs = 0;
            printf("  DIFF at abs %d: b3=%02x b4=%02x\n", i, f_b3[i], f_b4[i]);
        }
    }
    check(only_abs30_differs,
          "b3 vs b4 frames differ ONLY at abs30 (identity/nonce/incarnation/timer/"
          "markers unchanged -- vms-d94 keep-everything-else invariant)");
    /* Spot-check the fields vms-d94 must NOT disturb. */
    check(f_b4[92] == 0x01 && f_b4[93] == 0x00,
          "b4 frame: incarnation abs92 == 0x0001 unchanged (spec 4b/4i.B)");
    check(memcmp(f_b4 + 68, nonce, 4) == 0, "b4 frame: join nonce abs68 unchanged");
    check(memcmp(f_b4 + 24, ovmx_log, 6) == 0, "b4 frame: src-logical abs24 unchanged");
    check(f_b4[96] == 0x05 && f_b4[97] == 0x04, "b4 frame: live timer abs96 unchanged");
    check(f_b4[128] == 0x1f && f_b4[129] == 0x00, "b4 frame: poller-sweep abs128 unchanged");

    /* --- (4) drive the grounded handshake: reach b4, then toggle b3<->b4 --- */
    /* Bootstrap: member INIT b2 -> OVMX REQUEST b3 -> (member b3) -> OVMX CONFIRM b4. */
    uint8_t recv[5] = { SCS_HELLO_PFW_INIT, SCS_HELLO_PFW_REQUEST,
                        SCS_HELLO_PFW_CONFIRM, SCS_HELLO_PFW_REQUEST,
                        SCS_HELLO_PFW_CONFIRM };
    uint8_t expect[5] = { 0xb3, 0xb4, 0xb3, 0xb4, 0xb3 };
    int reached_b4 = 0, saw_b3 = 0, saw_b4 = 0;
    for (int i = 0; i < 5; i++) {
        uint8_t reply = scs_hello_response_pfw(recv[i]);
        uint8_t fr[SCS_HELLO_FRAME_LEN];
        check(scs_hello_build_directed_frame(&p, peer_mac, nonce, 1, reply, fr) == 0,
              "handshake step: build reply frame");
        check(fr[30] == expect[i], "handshake step: reply word matches grounded cadence");
        if (fr[30] == 0xb4) { reached_b4 = 1; saw_b4 = 1; }
        if (fr[30] == 0xb3) { saw_b3 = 1; }
    }
    check(reached_b4, "OVMX REACHES b4 during the handshake (no longer stuck at b3)");
    check(saw_b3 && saw_b4, "OVMX TOGGLES b3<->b4 across the exchange (grounded keepalive)");

    /* --- (5) vms-760: SCA dest-logical (abs 16) is the peer's CLUSTER-LOGICAL
     * addr, which is NOT the peer's HW MAC (the Ethernet dst, abs 0).
     * GROUNDED byte-exact on vax3-2to3-established-join-20260730.pcap frame 182
     * (VAX3 answering VAX2's channel probe):
     *     abs  0 = 08:00:2b:78:56:b9   VAX2's HW MAC
     *     abs 16 = aa:00:04:00:02:04   VAX2's LOGICAL addr
     *     abs 24 = aa:00:04:00:03:04   VAX3's own logical addr
     * Mirroring abs 0 into abs 16 (the pre-760 behaviour) makes every peer whose
     * HW MAC is not a DECnet aa:00:04:.. address DROP the reply: it re-probes
     * with b2 forever, never sends b4, and never opens SCS connections to the
     * joiner. That is invisible in a 2-node lab where VAX1's HW MAC IS its
     * logical addr, which is exactly how the bug survived. */
    {
        static const uint8_t v2_hw[6]  = { 0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9 };
        static const uint8_t v2_log[6] = { 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04 };
        check(memcmp(v2_hw, v2_log, 6) != 0,
              "vms-760: the reference peer's HW MAC and logical addr genuinely differ");

        struct scs_hello_params pl = p;
        memcpy(pl.peer_logical, v2_log, 6);
        uint8_t fl[SCS_HELLO_FRAME_LEN];
        check(scs_hello_build_directed_frame(&pl, v2_hw, nonce, 1,
                                             SCS_HELLO_PFW_REQUEST, fl) == 0,
              "vms-760: build directed HELLO with a distinct peer logical addr");
        check(memcmp(fl + 0, v2_hw, 6) == 0,
              "vms-760: Ethernet dst abs0 == peer HW MAC (ref frame 182)");
        check(memcmp(fl + 16, v2_log, 6) == 0,
              "vms-760: SCA dest-logical abs16 == peer LOGICAL addr (ref frame 182)");
        check(memcmp(fl + 16, fl + 0, 6) != 0,
              "vms-760: abs16 is NOT a mirror of abs0 when they differ");
        check(memcmp(fl + 24, ovmx_log, 6) == 0,
              "vms-760: abs24 still carries OUR OWN logical addr");

        /* All-zero peer_logical keeps the legacy mirror (correct only when the
         * peer's HW MAC equals its logical addr, e.g. VAX1). */
        struct scs_hello_params pz = p;
        memset(pz.peer_logical, 0, 6);
        uint8_t fz[SCS_HELLO_FRAME_LEN];
        check(scs_hello_build_directed_frame(&pz, v2_hw, nonce, 1,
                                             SCS_HELLO_PFW_REQUEST, fz) == 0,
              "vms-760: build directed HELLO with no peer logical addr supplied");
        check(memcmp(fz + 16, v2_hw, 6) == 0,
              "vms-760: absent peer_logical falls back to mirroring abs0");
    }

    if (failures == 0) {
        printf("\nALL PASS (vms-d94 abs-30 channel-verify rule)\n");
        return 0;
    }
    printf("\n%d FAILURE(S)\n", failures);
    return 1;
}
