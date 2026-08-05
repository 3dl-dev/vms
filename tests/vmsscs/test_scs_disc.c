/*
 * test_scs_disc.c - vms-591: the DISCONNECT_REQ / DISCONNECT_RSP builders.
 *
 * WHAT THIS FILE ASSERTS
 *
 * (1) BYTE-EXACT REPRODUCTION OF TWO REAL FRAMES. The four frames of one
 *     complete Figure 2-16 teardown are transcribed below verbatim from
 *     formation-ci1.pcap (raw frames 61/63/64/65, SCA #53/#55/#56/#57), with
 *     their pcap and index cited. The builders, driven with those frames' own
 *     parameters, must reproduce VAX2's two frames BYTE FOR BYTE across all 76
 *     and 72 bytes. That is what makes "captured template" a checkable claim
 *     and not a comment: if a substitution lands on the wrong offset, or the
 *     derived length fields stop agreeing with the capture, this reddens.
 *
 * (2) THE FOUR CAPTURED FRAMES ARE THE DIALOGUE THEY ARE CLAIMED TO BE.
 *     Asserted from the bytes, not from the prose: two DISCONNECT_REQ (message
 *     type 6 at payload [46:48], 62-byte SCA class) and two DISCONNECT_RSP
 *     (type 7, 58-byte class), with mirrored Con.ID pairs, one initiating
 *     ([60:62] = 0) and one matching ([60:62] = 1). This is the observation
 *     that overturns the spec's "5 and 7 do not exist on our wire", so it is a
 *     test rather than a sentence.
 *
 * (3) THE SUBSTITUTED FIELDS, individually: Con.ID pair, the three recv_ack
 *     copies and the two send_seq copies, the incarnation echo and its 0 =
 *     leave-the-template rule, the logical addresses, the reason code at the
 *     LABELED OVMX offset, and the matching flag -- each with a
 *     keep-everything-else invariant where that is the point.
 *
 * (4) THE DERIVED LENGTHS. [0:2] and [42:44] are recomputed, not copied, and
 *     the response's are asserted to differ from the request's by exactly the
 *     4 bytes of tail the 58-byte class does not have.
 *
 * (5) THE KILL SWITCH, RUN, bracketed on both sides -- and the suppressed call
 *     is asserted to have touched NO BYTE of the output buffer, not merely to
 *     have returned -1.
 *
 * WHAT IT DOES NOT ASSERT
 *
 * - That the [60:62] matching flag MEANS what scs_disc.h reads it as. What is
 *   measured is a partition with zero residuals over 220 frames
 *   (tools/cluster/scs_disc_measure.py); the meaning is inferred. This file
 *   asserts the encoding OVMX emits, which is all a unit test can see.
 * - That the reason-code OFFSET is where VMS puts it. It is not known where
 *   VMS puts it (scs_reason.h); every VMS DISCONNECT_REQ we hold reads zero
 *   there, and frames (1) and (2) below are two of those.
 * - Anything about the daemon. The production dialogue -- answering a peer's
 *   DISCONNECT_REQ, invoking the symmetric own-disconnect, the simultaneous
 *   case and the shutdown teardown -- is covered where it lives, over the real
 *   scsd.c translation unit, in tests/vmsscs/test_scsd_wire.c.
 */
#include "scs_disc.h"
#include "scs_reason.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        checks++;                                                                        \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            printf("FAIL %s:%d: ", __func__, __LINE__);                                  \
            printf(__VA_ARGS__);                                                         \
            printf("\n");                                                                \
        }                                                                                \
    } while (0)

/* ------------------------------------------------------------------------- *
 * ONE COMPLETE FIGURE 2-16 TEARDOWN, BYTE-EXACT FROM THE LAB WIRE.
 * formation-ci1.pcap, raw frames 61/63/64/65 (SCA #53/#55/#56/#57), between
 * VAX1 (Con.ID 0x63050008, MAC aa:00:04:00:01:04) and VAX2 (Con.ID
 * 0x33590007, MAC 08:00:2b:78:56:b9).
 * ------------------------------------------------------------------------- */

/* #53  VAX1 -> VAX2   DISCONNECT_REQ, the INITIATING one ([60:62] = 0). */
static const uint8_t cap_req_initiating[SCS_DISC_REQ_FRAME_LEN] = {
    0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9, 0xaa, 0x00,
    0x04, 0x00, 0x01, 0x04, 0x60, 0x07, 0x3c, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x4b, 0x13,
    0x0d, 0x00, 0x0d, 0x00, 0x01, 0x00, 0x12, 0x00,
    0x0d, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00,
    0x0d, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02,
    0x12, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x07, 0x00, 0x59, 0x33, 0x08, 0x00, 0x05, 0x63,
    0x00, 0x00, 0x00, 0x00
};

/* #55  VAX2 -> VAX1   DISCONNECT_RSP -- THE TEMPLATE scs_disc.c replays. */
static const uint8_t cap_rsp_vax2[SCS_DISC_RSP_FRAME_LEN] = {
    0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x08, 0x00,
    0x2b, 0x78, 0x56, 0xb9, 0x60, 0x07, 0x38, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x4b, 0x13,
    0x0d, 0x00, 0x0e, 0x00, 0x01, 0x00, 0x12, 0x00,
    0x0d, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00,
    0x0d, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02,
    0x0e, 0x00, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x05, 0x63, 0x07, 0x00, 0x59, 0x33
};

/* #56  VAX2 -> VAX1   DISCONNECT_REQ, the MATCHING one ([60:62] = 1) --
 *                     THE TEMPLATE scs_disc.c replays. */
static const uint8_t cap_req_matching[SCS_DISC_REQ_FRAME_LEN] = {
    0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x08, 0x00,
    0x2b, 0x78, 0x56, 0xb9, 0x60, 0x07, 0x3c, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x4b, 0x13,
    0x0e, 0x00, 0x0f, 0x00, 0x01, 0x00, 0x12, 0x00,
    0x0e, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00,
    0x0e, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02,
    0x12, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x05, 0x63, 0x07, 0x00, 0x59, 0x33,
    0x00, 0x00, 0x01, 0x00
};

/* #57  VAX1 -> VAX2   DISCONNECT_RSP, closing the dialogue. */
static const uint8_t cap_rsp_vax1[SCS_DISC_RSP_FRAME_LEN] = {
    0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9, 0xaa, 0x00,
    0x04, 0x00, 0x01, 0x04, 0x60, 0x07, 0x38, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x4b, 0x13,
    0x0f, 0x00, 0x0f, 0x00, 0x01, 0x00, 0x12, 0x00,
    0x0f, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00,
    0x0f, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02,
    0x0e, 0x00, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00,
    0x07, 0x00, 0x59, 0x33, 0x08, 0x00, 0x05, 0x63
};

static const uint8_t vax1_mac[6] = {0xaa, 0x00, 0x04, 0x00, 0x01, 0x04};
static const uint8_t vax2_mac[6] = {0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9};
static const uint8_t vax1_log[6] = {0xaa, 0x00, 0x04, 0x00, 0x01, 0x04};
static const uint8_t vax2_log[6] = {0xaa, 0x00, 0x04, 0x00, 0x02, 0x04};

#define VAX1_CONID 0x63050008u
#define VAX2_CONID 0x33590007u

static uint16_t le16(const uint8_t *f, size_t off)
{
    return (uint16_t)((unsigned)f[off] | ((unsigned)f[off + 1] << 8));
}

static uint32_t le32(const uint8_t *f, size_t off)
{
    return (uint32_t)f[off] | ((uint32_t)f[off + 1] << 8) |
           ((uint32_t)f[off + 2] << 16) | ((uint32_t)f[off + 3] << 24);
}

static void diffdump(const char *what, const uint8_t *got, const uint8_t *want, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (got[i] != want[i]) {
            printf("      %s: first difference at abs %zu: got 0x%02x want 0x%02x\n",
                   what, i, got[i], want[i]);
            return;
        }
    }
}

/* VAX2's parameters, as read off the two frames it sent. */
static struct scs_disc_params vax2_params(void)
{
    struct scs_disc_params p;
    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, vax1_mac, 6);
    memcpy(p.src_mac, vax2_mac, 6);
    memcpy(p.src_logical, vax2_log, 6);
    memcpy(p.peer_logical, vax1_log, 6);
    p.remote_conid = VAX1_CONID;
    p.local_conid = VAX2_CONID;
    p.incarnation = 1;
    p.reason = SCS_REASON_NONE;
    return p;
}

/* ---- (2) the four captured frames really are the dialogue ---------------- */

static void test_the_captured_dialogue_is_what_it_is_claimed_to_be(void)
{
    /* Message types, read off the wire. */
    CHECK(le16(cap_req_initiating, 14 + 46) == SCS_DISC_MSGTYPE_REQ,
          "cap #53 [46:48] = %u, expected DISCONNECT_REQ (6)",
          le16(cap_req_initiating, 14 + 46));
    CHECK(le16(cap_req_matching, 14 + 46) == SCS_DISC_MSGTYPE_REQ,
          "cap #56 [46:48] = %u, expected DISCONNECT_REQ (6)",
          le16(cap_req_matching, 14 + 46));
    CHECK(le16(cap_rsp_vax2, 14 + 46) == SCS_DISC_MSGTYPE_RSP,
          "cap #55 [46:48] = %u, expected DISCONNECT_RSP (7)",
          le16(cap_rsp_vax2, 14 + 46));
    CHECK(le16(cap_rsp_vax1, 14 + 46) == SCS_DISC_MSGTYPE_RSP,
          "cap #57 [46:48] = %u, expected DISCONNECT_RSP (7)",
          le16(cap_rsp_vax1, 14 + 46));

    /* THE CLAIM THE SPEC HAD TO BE CORRECTED FOR: the RESPONSE is a SHORTER
     * length class than any of the four formation messages, which is why every
     * census restricted to {62, 66, 110} missed it. */
    CHECK(le16(cap_rsp_vax2, 14) + 2 == SCS_DISC_RSP_SCA_LEN,
          "cap #55 SCA length %u, expected %d", le16(cap_rsp_vax2, 14) + 2,
          SCS_DISC_RSP_SCA_LEN);
    CHECK(le16(cap_req_initiating, 14) + 2 == SCS_DISC_REQ_SCA_LEN,
          "cap #53 SCA length %u, expected %d", le16(cap_req_initiating, 14) + 2,
          SCS_DISC_REQ_SCA_LEN);
    CHECK(SCS_DISC_RSP_SCA_LEN < 62,
          "the response class is not shorter than the smallest class the old "
          "census looked at -- the sampling explanation does not hold");

    /* Mirrored Con.ID pairs: each frame's [50:54]/[54:58] is the other
     * direction's pair swapped. */
    CHECK(le32(cap_req_initiating, 14 + 50) == VAX2_CONID &&
              le32(cap_req_initiating, 14 + 54) == VAX1_CONID,
          "cap #53 Con.ID pair is not VAX1->VAX2");
    CHECK(le32(cap_rsp_vax2, 14 + 50) == VAX1_CONID &&
              le32(cap_rsp_vax2, 14 + 54) == VAX2_CONID,
          "cap #55 Con.ID pair is not the mirror of #53");

    /* The matching flag partition, on the two real frames. */
    uint16_t m = 0xffff;
    CHECK(scs_disc_match_get(cap_req_initiating, sizeof(cap_req_initiating), &m) == 1 &&
              m == SCS_DISC_MATCH_INITIAL,
          "cap #53 (the first DISCONNECT_REQ of the dialogue) [60:62] = 0x%04x, "
          "expected 0x%04x", m, SCS_DISC_MATCH_INITIAL);
    CHECK(scs_disc_match_get(cap_req_matching, sizeof(cap_req_matching), &m) == 1 &&
              m == SCS_DISC_MATCH_MATCHING,
          "cap #56 (the matching DISCONNECT_REQ) [60:62] = 0x%04x, expected 0x%04x",
          m, SCS_DISC_MATCH_MATCHING);
    /* A RESPONSE has no such field -- the frame ends before it. */
    CHECK(scs_disc_match_get(cap_rsp_vax2, sizeof(cap_rsp_vax2), &m) == 0,
          "scs_disc_match_get() claimed to read a matching flag out of a "
          "DISCONNECT_RSP, which is 4 bytes too short to have one");

    /* Every VMS DISCONNECT_REQ we hold reads zero at the LABELED OVMX reason
     * offset. These two are among them; the whole placement rests on it. */
    uint16_t r = 0xffff;
    CHECK(scs_reason_get(cap_req_initiating, sizeof(cap_req_initiating),
                         SCS_REASON_MSGTYPE_DISCONNECT_REQ, &r) == 1 && r == 0,
          "cap #53 reason slot = %u, expected 0", r);
    CHECK(scs_reason_get(cap_req_matching, sizeof(cap_req_matching),
                         SCS_REASON_MSGTYPE_DISCONNECT_REQ, &r) == 1 && r == 0,
          "cap #56 reason slot = %u, expected 0", r);
}

/* ---- (1) byte-exact reproduction ---------------------------------------- */

static void test_request_reproduces_the_captured_frame(void)
{
    struct scs_disc_params p = vax2_params();
    uint8_t out[SCS_DISC_REQ_FRAME_LEN];

    p.recv_ack = 0x000e;
    p.send_seq = 0x000f;
    p.matching = 1; /* #56 is the matching half */
    memset(out, 0xa5, sizeof(out));
    CHECK(scs_disc_build_request(&p, out) == 0, "build_request failed");
    CHECK(memcmp(out, cap_req_matching, sizeof(out)) == 0,
          "the built DISCONNECT_REQ is not byte-identical to formation-ci1.pcap "
          "raw frame 64 (SCA #56)");
    diffdump("REQ", out, cap_req_matching, sizeof(out));
}

static void test_response_reproduces_the_captured_frame(void)
{
    struct scs_disc_params p = vax2_params();
    uint8_t out[SCS_DISC_RSP_FRAME_LEN];

    p.recv_ack = 0x000d;
    p.send_seq = 0x000e;
    memset(out, 0xa5, sizeof(out));
    CHECK(scs_disc_build_response(&p, out) == 0, "build_response failed");
    CHECK(memcmp(out, cap_rsp_vax2, sizeof(out)) == 0,
          "the built DISCONNECT_RSP is not byte-identical to formation-ci1.pcap "
          "raw frame 63 (SCA #55)");
    diffdump("RSP", out, cap_rsp_vax2, sizeof(out));
}

/* OVMX can also reproduce the OTHER end's frames, which is the real check that
 * the substitutions are parameters and not the template leaking through: same
 * builders, VAX1's arguments, VAX1's two frames. */
static void test_the_other_end_is_reproducible_too(void)
{
    struct scs_disc_params p;
    uint8_t req[SCS_DISC_REQ_FRAME_LEN];
    uint8_t rsp[SCS_DISC_RSP_FRAME_LEN];

    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, vax2_mac, 6);
    memcpy(p.src_mac, vax1_mac, 6);
    memcpy(p.src_logical, vax1_log, 6);
    memcpy(p.peer_logical, vax2_log, 6);
    p.remote_conid = VAX2_CONID;
    p.local_conid = VAX1_CONID;
    p.incarnation = 1;

    p.recv_ack = 0x000d;
    p.send_seq = 0x000d;
    p.matching = 0; /* #53 initiated the dialogue */
    CHECK(scs_disc_build_request(&p, req) == 0, "build_request failed");
    CHECK(memcmp(req, cap_req_initiating, sizeof(req)) == 0,
          "the built DISCONNECT_REQ is not byte-identical to raw frame 61 "
          "(SCA #53), VAX1's initiating request");
    diffdump("REQ(vax1)", req, cap_req_initiating, sizeof(req));

    p.recv_ack = 0x000f;
    p.send_seq = 0x000f;
    CHECK(scs_disc_build_response(&p, rsp) == 0, "build_response failed");
    CHECK(memcmp(rsp, cap_rsp_vax1, sizeof(rsp)) == 0,
          "the built DISCONNECT_RSP is not byte-identical to raw frame 65 "
          "(SCA #57), VAX1's closing response");
    diffdump("RSP(vax1)", rsp, cap_rsp_vax1, sizeof(rsp));
}

/* ---- (3) the substituted fields ----------------------------------------- */

static void test_conids_and_addresses_land_where_they_are_claimed_to(void)
{
    struct scs_disc_params p = vax2_params();
    uint8_t out[SCS_DISC_REQ_FRAME_LEN];

    p.remote_conid = 0xDEADBEEFu;
    p.local_conid = 0x4F580042u;
    CHECK(scs_disc_build_request(&p, out) == 0, "build failed");
    CHECK(le32(out, 14 + 50) == 0xDEADBEEFu, "remote Con.ID not at payload [50:54]");
    CHECK(le32(out, 14 + 54) == 0x4F580042u, "local Con.ID not at payload [54:58]");
    CHECK(memcmp(out + 0, vax1_mac, 6) == 0, "Ethernet dst not the peer MAC");
    CHECK(memcmp(out + 6, vax2_mac, 6) == 0, "Ethernet src not our MAC");
    CHECK(out[12] == 0x60 && out[13] == 0x07, "ethertype is not 0x6007");
    CHECK(memcmp(out + 14 + 2, vax1_log, 6) == 0,
          "dest LOGICAL address not at payload [2:8]");
    CHECK(memcmp(out + 14 + 10, vax2_log, 6) == 0,
          "src LOGICAL address not at payload [10:16]");
}

static void test_counters_land_in_all_five_slots(void)
{
    struct scs_disc_params p = vax2_params();
    uint8_t req[SCS_DISC_REQ_FRAME_LEN];
    uint8_t rsp[SCS_DISC_RSP_FRAME_LEN];

    p.recv_ack = 0x1234;
    p.send_seq = 0x5678;
    CHECK(scs_disc_build_request(&p, req) == 0, "build failed");
    CHECK(le16(req, 14 + 18) == 0x1234, "recv_ack not at [18:20]");
    CHECK(le16(req, 14 + 26) == 0x1234, "recv_ack not mirrored at [26:28]");
    CHECK(le16(req, 14 + 34) == 0x1234, "recv_ack not mirrored at [34:36]");
    CHECK(le16(req, 14 + 20) == 0x5678, "send_seq not at [20:22]");
    CHECK(le16(req, 14 + 30) == 0x5678, "send_seq not mirrored at [30:32]");

    /* The RESPONSE carries the same five, at the same offsets. */
    CHECK(scs_disc_build_response(&p, rsp) == 0, "build failed");
    CHECK(le16(rsp, 14 + 18) == 0x1234 && le16(rsp, 14 + 26) == 0x1234 &&
              le16(rsp, 14 + 34) == 0x1234,
          "the response's three recv_ack copies are not all 0x1234");
    CHECK(le16(rsp, 14 + 20) == 0x5678 && le16(rsp, 14 + 30) == 0x5678,
          "the response's two send_seq copies are not both 0x5678");
}

static void test_incarnation_echo_and_its_zero_rule(void)
{
    struct scs_disc_params p = vax2_params();
    uint8_t out[SCS_DISC_REQ_FRAME_LEN];

    p.incarnation = 3;
    CHECK(scs_disc_build_request(&p, out) == 0, "build failed");
    CHECK(le16(out, 14 + 22) == 3, "incarnation echo not at [22:24]");

    /* 0 = leave the template's fresh-contact value 1 (same rule as scs_dir.c
     * and scs_connect.c, so all three agree). */
    p.incarnation = 0;
    CHECK(scs_disc_build_request(&p, out) == 0, "build failed");
    CHECK(le16(out, 14 + 22) == 1,
          "incarnation 0 did not leave the template's 1 at [22:24] (got %u)",
          le16(out, 14 + 22));
}

static void test_matching_flag_changes_exactly_two_bytes(void)
{
    struct scs_disc_params p = vax2_params();
    uint8_t a[SCS_DISC_REQ_FRAME_LEN];
    uint8_t b[SCS_DISC_REQ_FRAME_LEN];

    p.matching = 0;
    CHECK(scs_disc_build_request(&p, a) == 0, "build failed");
    p.matching = 1;
    CHECK(scs_disc_build_request(&p, b) == 0, "build failed");

    CHECK(le16(a, SCS_DISC_MATCH_FRAME_OFF) == SCS_DISC_MATCH_INITIAL,
          "matching=0 did not write 0x%04x", SCS_DISC_MATCH_INITIAL);
    CHECK(le16(b, SCS_DISC_MATCH_FRAME_OFF) == SCS_DISC_MATCH_MATCHING,
          "matching=1 did not write 0x%04x", SCS_DISC_MATCH_MATCHING);

    /* The KEEP-EVERYTHING-ELSE invariant. The flag only ever takes 0 or 1, so
     * only its low byte can differ between the two frames -- what has to be
     * asserted is that NO byte outside the two-byte field moves, and that at
     * least one inside it does. Pinning a diff COUNT of 2 here would be
     * asserting a property of the two values chosen, not of the encoder. */
    unsigned inside = 0;
    unsigned outside = 0;
    for (size_t i = 0; i < sizeof(a); i++) {
        if (a[i] == b[i]) {
            continue;
        }
        if (i == SCS_DISC_MATCH_FRAME_OFF || i == SCS_DISC_MATCH_FRAME_OFF + 1) {
            inside++;
        } else {
            outside++;
            CHECK(0, "the matching flag changed byte abs %zu, which is not [60:62]", i);
        }
    }
    CHECK(inside > 0, "flipping the matching flag changed no byte of [60:62]");
    CHECK(outside == 0, "flipping the matching flag changed %u byte(s) elsewhere", outside);
}

static void test_reason_code_changes_exactly_two_bytes_and_only_on_the_request(void)
{
    (void)unsetenv("OVMX_NO_REASON_CODE");
    struct scs_disc_params p = vax2_params();
    uint8_t a[SCS_DISC_REQ_FRAME_LEN];
    uint8_t b[SCS_DISC_REQ_FRAME_LEN];
    uint8_t r0[SCS_DISC_RSP_FRAME_LEN];
    uint8_t r1[SCS_DISC_RSP_FRAME_LEN];

    /* 0x0102 is deliberately a value whose HIGH byte is also nonzero, so this
     * case exercises BOTH bytes of the field and its little-endian order. The
     * value is not one of enum scs_reason_code, which does not matter -- the
     * codec carries 16 bits and never interprets them. */
    p.reason = SCS_REASON_NONE;
    CHECK(scs_disc_build_request(&p, a) == 0, "build failed");
    CHECK(scs_disc_build_response(&p, r0) == 0, "build failed");
    p.reason = 0x0102;
    CHECK(scs_disc_build_request(&p, b) == 0, "build failed");
    CHECK(scs_disc_build_response(&p, r1) == 0, "build failed");

    CHECK(le16(b, SCS_REASON_FRAME_OFF) == 0x0102,
          "the reason code is not at the offset scs_reason.h defines");
    CHECK(b[SCS_REASON_FRAME_OFF] == 0x02 && b[SCS_REASON_FRAME_OFF + 1] == 0x01,
          "the reason code is not little-endian at [58:60] (got %02x %02x)",
          b[SCS_REASON_FRAME_OFF], b[SCS_REASON_FRAME_OFF + 1]);

    unsigned diffs = 0;
    for (size_t i = 0; i < sizeof(a); i++) {
        if (a[i] != b[i]) {
            diffs++;
            CHECK(i == SCS_REASON_FRAME_OFF || i == SCS_REASON_FRAME_OFF + 1,
                  "the reason code changed byte abs %zu, which is not [58:60]", i);
        }
    }
    CHECK(diffs == 2, "changing the reason changed %u bytes, expected 2", diffs);

    /* And a one-byte value moves exactly one byte -- the control that says the
     * count above is a property of the VALUE, not slack in the encoder. */
    p.reason = SCS_REASON_SYSAP_SHUTDOWN;
    CHECK(scs_disc_build_request(&p, b) == 0, "build failed");
    CHECK(le16(b, SCS_REASON_FRAME_OFF) == SCS_REASON_SYSAP_SHUTDOWN,
          "the reason code is not at the offset scs_reason.h defines");

    /* THE RESPONSE HAS NO REASON FIELD. The 58-byte class ends at the Con.ID
     * pair; writing one would be inventing a field, so the reason must be
     * invisible in the response. */
    CHECK(memcmp(r0, r1, sizeof(r0)) == 0,
          "the reason code changed the DISCONNECT_RSP -- the 58-byte class has "
          "no room for one and none may be written");
}

/*
 * THE OTHER SWITCH. vms-6b3's OVMX_NO_REASON_CODE must gate the reason code in
 * THIS frame too -- OVMX's DISCONNECT_REQ is the only frame it ever sets the
 * field on, so a builder that wrote the bytes itself would have left that
 * switch unable to gate the one thing it exists for. RUN, with controls.
 */
static void test_the_reason_code_kill_switch_reaches_this_frame(void)
{
    struct scs_disc_params p = vax2_params();
    uint8_t out[SCS_DISC_REQ_FRAME_LEN];

    p.reason = SCS_REASON_SYSAP_SHUTDOWN;

    /* CONTROL BEFORE. */
    (void)unsetenv("OVMX_NO_REASON_CODE");
    CHECK(scs_disc_build_request(&p, out) == 0, "control: build failed");
    CHECK(le16(out, SCS_REASON_FRAME_OFF) == SCS_REASON_SYSAP_SHUTDOWN,
          "control: the reason code was not written");

    /* THE SWITCH. */
    (void)setenv("OVMX_NO_REASON_CODE", "1", 1);
    CHECK(scs_disc_build_request(&p, out) == 0, "build failed with the switch set");
    CHECK(le16(out, SCS_REASON_FRAME_OFF) == 0,
          "OVMX_NO_REASON_CODE DID NOT GATE THE DISCONNECT_REQ's reason code "
          "(got %u) -- the frame must fall back to the captured template's "
          "0x0000, which is what every VMS DISCONNECT_REQ we hold carries",
          le16(out, SCS_REASON_FRAME_OFF));
    /* And it gates ONLY that field: the rest of the frame is untouched. */
    {
        struct scs_disc_params q = p;
        uint8_t ref[SCS_DISC_REQ_FRAME_LEN];
        q.reason = SCS_REASON_NONE;
        (void)unsetenv("OVMX_NO_REASON_CODE");
        CHECK(scs_disc_build_request(&q, ref) == 0, "build failed");
        (void)setenv("OVMX_NO_REASON_CODE", "1", 1);
        CHECK(scs_disc_build_request(&p, out) == 0, "build failed");
        CHECK(memcmp(out, ref, sizeof(out)) == 0,
              "with the reason switch set the frame is not identical to the "
              "same frame built with reason NONE -- the switch moved something "
              "other than [58:60]");
    }

    /* CONTROL AFTER. */
    (void)unsetenv("OVMX_NO_REASON_CODE");
    CHECK(scs_disc_build_request(&p, out) == 0, "control-after: build failed");
    CHECK(le16(out, SCS_REASON_FRAME_OFF) == SCS_REASON_SYSAP_SHUTDOWN,
          "control-after: clearing the switch did not restore the reason code");
}

/* ---- (4) the derived lengths -------------------------------------------- */

static void test_lengths_are_derived_and_consistent(void)
{
    struct scs_disc_params p = vax2_params();
    uint8_t req[SCS_DISC_REQ_FRAME_LEN];
    uint8_t rsp[SCS_DISC_RSP_FRAME_LEN];

    CHECK(scs_disc_build_request(&p, req) == 0, "build failed");
    CHECK(scs_disc_build_response(&p, rsp) == 0, "build failed");

    CHECK(le16(req, 14) + 2 == SCS_DISC_REQ_SCA_LEN,
          "request [0:2] = %u, expected %d - 2", le16(req, 14), SCS_DISC_REQ_SCA_LEN);
    CHECK(le16(rsp, 14) + 2 == SCS_DISC_RSP_SCA_LEN,
          "response [0:2] = %u, expected %d - 2", le16(rsp, 14), SCS_DISC_RSP_SCA_LEN);
    /* spec sec 4h: inner length = payload_len - 44. */
    CHECK(le16(req, 14 + 42) == SCS_DISC_REQ_SCA_LEN - 44,
          "request inner length [42:44] = %u, expected %d", le16(req, 14 + 42),
          SCS_DISC_REQ_SCA_LEN - 44);
    CHECK(le16(rsp, 14 + 42) == SCS_DISC_RSP_SCA_LEN - 44,
          "response inner length [42:44] = %u, expected %d", le16(rsp, 14 + 42),
          SCS_DISC_RSP_SCA_LEN - 44);
    CHECK(SCS_DISC_REQ_FRAME_LEN - SCS_DISC_RSP_FRAME_LEN == 4,
          "the two classes do not differ by exactly the 4-byte tail");

    /* THE SHAPE CLAIM: the two frames are the SAME ENVELOPE, and the only
     * things that may differ before the tail are the two length fields and the
     * message type. Asserted as a membership test over every differing byte,
     * not as a diff count -- a count would be a property of the particular
     * values (all three fields happen to differ in their low byte only). */
    static const size_t allowed[] = {14, 15, 14 + 42, 14 + 43, 14 + 46, 14 + 47};
    unsigned diffs = 0;
    for (size_t i = 0; i < SCS_DISC_RSP_FRAME_LEN; i++) {
        if (req[i] == rsp[i]) {
            continue;
        }
        diffs++;
        int ok = 0;
        for (size_t k = 0; k < sizeof(allowed) / sizeof(allowed[0]); k++) {
            if (i == allowed[k]) {
                ok = 1;
            }
        }
        CHECK(ok, "request and response differ at abs %zu, which is neither a "
                  "length field nor the message type", i);
    }
    CHECK(diffs == 3,
          "request and response differ in %u byte(s) before the tail; expected "
          "3 -- the low byte of the SCA length, of the inner length and of the "
          "message type (all three high bytes are 0 in both frames)", diffs);
}

/* ---- (5) the kill switch, RUN ------------------------------------------- */

static void test_kill_switch(void)
{
    struct scs_disc_params p = vax2_params();
    uint8_t out[SCS_DISC_REQ_FRAME_LEN];
    uint8_t rsp[SCS_DISC_RSP_FRAME_LEN];

    /* CONTROL BEFORE: the switch is clear and both builders work. */
    unsetenv("OVMX_NO_CLEAN_SHUTDOWN");
    CHECK(scs_disc_enabled() == 1, "control: expected enabled with the switch clear");
    CHECK(scs_disc_build_request(&p, out) == 0, "control: request build failed");
    CHECK(scs_disc_build_response(&p, rsp) == 0, "control: response build failed");

    /* THE SWITCH. Fill the buffers with a poison pattern first, so "nothing
     * happened" is checkable as bytes and not merely as a return code. */
    setenv("OVMX_NO_CLEAN_SHUTDOWN", "1", 1);
    CHECK(scs_disc_enabled() == 0, "the switch did not disable the builders");
    memset(out, 0x5a, sizeof(out));
    memset(rsp, 0x5a, sizeof(rsp));
    CHECK(scs_disc_build_request(&p, out) == -1,
          "OVMX_NO_CLEAN_SHUTDOWN=1 did not refuse the request build");
    CHECK(scs_disc_build_response(&p, rsp) == -1,
          "OVMX_NO_CLEAN_SHUTDOWN=1 did not refuse the response build");
    unsigned touched = 0;
    for (size_t i = 0; i < sizeof(out); i++) {
        if (out[i] != 0x5a) {
            touched++;
        }
    }
    for (size_t i = 0; i < sizeof(rsp); i++) {
        if (rsp[i] != 0x5a) {
            touched++;
        }
    }
    CHECK(touched == 0,
          "the suppressed builders wrote %u byte(s) into the output buffer -- "
          "a refused build must leave the caller's buffer alone", touched);

    /* "0" is not "set". */
    setenv("OVMX_NO_CLEAN_SHUTDOWN", "0", 1);
    CHECK(scs_disc_enabled() == 1, "OVMX_NO_CLEAN_SHUTDOWN=0 disabled the builders");

    /* CONTROL AFTER: clearing it restores the byte-exact capture. */
    unsetenv("OVMX_NO_CLEAN_SHUTDOWN");
    p.recv_ack = 0x000e;
    p.send_seq = 0x000f;
    p.matching = 1;
    CHECK(scs_disc_build_request(&p, out) == 0, "control-after: build failed");
    CHECK(memcmp(out, cap_req_matching, sizeof(out)) == 0,
          "control-after: clearing the switch did not restore the captured frame");
}

static void test_null_arguments(void)
{
    struct scs_disc_params p = vax2_params();
    uint8_t out[SCS_DISC_REQ_FRAME_LEN];
    uint16_t m = 0;

    CHECK(scs_disc_build_request(NULL, out) == -1, "NULL params accepted");
    CHECK(scs_disc_build_request(&p, NULL) == -1, "NULL out accepted");
    CHECK(scs_disc_build_response(NULL, out) == -1, "NULL params accepted");
    CHECK(scs_disc_build_response(&p, NULL) == -1, "NULL out accepted");
    CHECK(scs_disc_match_get(NULL, 76, &m) == 0, "NULL frame accepted");
    CHECK(scs_disc_match_get(cap_req_matching, 8, &m) == 0, "short frame accepted");
}

int main(void)
{
    printf("test_scs_disc: vms-591 DISCONNECT_REQ / DISCONNECT_RSP builders\n");
    unsetenv("OVMX_NO_CLEAN_SHUTDOWN");

    test_the_captured_dialogue_is_what_it_is_claimed_to_be();
    test_request_reproduces_the_captured_frame();
    test_response_reproduces_the_captured_frame();
    test_the_other_end_is_reproducible_too();
    test_conids_and_addresses_land_where_they_are_claimed_to();
    test_counters_land_in_all_five_slots();
    test_incarnation_echo_and_its_zero_rule();
    test_matching_flag_changes_exactly_two_bytes();
    test_reason_code_changes_exactly_two_bytes_and_only_on_the_request();
    test_the_reason_code_kill_switch_reaches_this_frame();
    test_lengths_are_derived_and_consistent();
    test_kill_switch();
    test_null_arguments();

    printf("%s: %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
