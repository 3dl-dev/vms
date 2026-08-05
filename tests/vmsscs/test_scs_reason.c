/*
 * test_scs_reason.c - vms-6b3: the 16-bit REJECT/DISCONNECT reason code codec.
 *
 * WHAT THIS FILE ASSERTS, AND WHAT IT DELIBERATELY DOES NOT.
 *
 * ASSERTS (1): the two REAL captured frames below -- one REJECT_REQ, one
 * DISCONNECT_REQ, transcribed byte-exact from the lab captures with their pcap
 * and frame index cited -- carry message type 4 and 6 at payload [46:48], and
 * read ZERO at the reason-code slot. That is the observation the whole
 * placement rests on, so it is a test and not a sentence: if anyone moves
 * SCS_REASON_PAYLOAD_OFF onto a byte real VMS frames use, this reddens.
 *
 * ASSERTS (2): byte position, little-endian order and round-trip of the codec,
 * plus a keep-everything-else invariant (a put changes EXACTLY the two bytes at
 * SCS_REASON_FRAME_OFF and no other byte of the frame).
 *
 * ASSERTS (3): the OVMX_NO_REASON_CODE kill switch, RUN, with a bracketing
 * control on both sides -- and the suppressed put is asserted to have touched
 * NO BYTE, not merely to have returned 0.
 *
 * DOES NOT ASSERT: that the offset is where VMS puts it. It is not known where
 * VMS puts it and this test cannot discover that; see scs_reason.h and
 * docs/cluster-protocol-spec.md sec 5. Nothing here is evidence about VMS
 * beyond "every VMS frame we hold reads zero there", which is exactly what
 * assertion (1) says and no more.
 *
 * DOES NOT ASSERT: that OVMX transmits a reason code. It does not -- OVMX
 * builds neither REJECT_REQ nor DISCONNECT_REQ (scs_svc.h). scs_reason_put()
 * has no production caller and this file is its only caller.
 *
 * The RECEIVE half, which IS live in the daemon, is covered where it lives:
 * tests/vmsscs/test_scsd_wire.c, the four test_reason_* cases.
 */
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

/*
 * REAL FRAME 1 -- a VMS REJECT_REQ.
 * ovmx-e81-bystander-ADDITION-SUCCESS-20260731.pcap, SCA frame index 4873.
 * Source 08:00:2b:11:22:33 (VAX3, DEC OUI). Total SCA length 62 (the
 * connection-control class, spec sec 4(h)(1a)); message type 4 at payload
 * [46:48]; destination Con.ID 0x4F58000A at payload [50:54].
 */
static const uint8_t cap_reject_req[76] = {
    0xb6, 0x16, 0x8a, 0xdc, 0x3a, 0x53, 0x08, 0x00,
    0x2b, 0x11, 0x22, 0x33, 0x60, 0x07, 0x3c, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0xb9, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x03, 0x04, 0x4b, 0x13,
    0x17, 0x00, 0x18, 0x00, 0x01, 0x00, 0x12, 0x00,
    0x17, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
    0x17, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02,
    0x12, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x0a, 0x00, 0x58, 0x4f, 0x0e, 0x00, 0x10, 0x1f,
    0x00, 0x00, 0x01, 0x00,
};

/*
 * REAL FRAME 2 -- a VMS DISCONNECT_REQ.
 * ovmx-760-MEMBER-achieved-20260730.pcap, SCA frame index 181.
 * Source 08:00:2b:78:56:b9 (VAX2, DEC OUI). Message type 6 at payload [46:48];
 * destination Con.ID 0x4F580007 -- OVMX's own SCS$DIRECTORY handle, i.e. this
 * is a real VMS node disconnecting a real connection to OVMX.
 */
static const uint8_t cap_disconnect_req[76] = {
    0xb6, 0x16, 0x8a, 0xdc, 0x3a, 0x53, 0x08, 0x00,
    0x2b, 0x78, 0x56, 0xb9, 0x60, 0x07, 0x3c, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x9b, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x4b, 0x13,
    0x18, 0x00, 0x19, 0x00, 0x01, 0x00, 0x12, 0x00,
    0x18, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00,
    0x18, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02,
    0x12, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x07, 0x00, 0x58, 0x4f, 0x12, 0x00, 0x02, 0x63,
    0x00, 0x00, 0x00, 0x00,
};

static unsigned msgtype_of(const uint8_t *f)
{
    return (unsigned)f[60] | ((unsigned)f[61] << 8);
}

/* (1) The captured frames are what the header says they are. */
static void test_the_captured_frames_are_the_two_carriers(void)
{
    CHECK(cap_reject_req[12] == 0x60 && cap_reject_req[13] == 0x07,
          "the REJECT_REQ specimen is not an SCA frame");
    CHECK(((unsigned)cap_reject_req[14] | ((unsigned)cap_reject_req[15] << 8)) + 2u == 62u,
          "the REJECT_REQ specimen is not in the 62-byte connection-control class");
    CHECK(msgtype_of(cap_reject_req) == SCS_REASON_MSGTYPE_REJECT_REQ,
          "the REJECT_REQ specimen carries message type %u, expected 4",
          msgtype_of(cap_reject_req));
    CHECK(msgtype_of(cap_disconnect_req) == SCS_REASON_MSGTYPE_DISCONNECT_REQ,
          "the DISCONNECT_REQ specimen carries message type %u, expected 6",
          msgtype_of(cap_disconnect_req));
    CHECK(scs_reason_carried_by(msgtype_of(cap_reject_req)) == 1 &&
          scs_reason_carried_by(msgtype_of(cap_disconnect_req)) == 1,
          "the two carriers are not recognised as carriers");
}

/*
 * (1, the load-bearing half) THE PLACEMENT OBSERVATION, AS A TEST.
 *
 * Both real VMS frames read 0x0000 at the OVMX reason-code slot. This is the
 * single-frame form of the CENSUS-A lines in scs_reason.h -- the population
 * counts live THERE and nowhere else, because a figure repeated in a second
 * comment is exactly how the refuted rationale of review round 2 survived; see
 * tests/vmsscs/test_scs_reason_figures.py. Re-derive with
 * tools/cluster/scs_reason_measure.py. This is NOT a claim that VMS puts its
 * reason code here -- it is the claim that OVMX writing zero here is
 * byte-identical to what VMS sends, which is the only property the placement
 * needs and the only one that can be checked.
 */
static void test_real_vms_frames_read_zero_at_the_slot(void)
{
    uint16_t r = 0xbeef;
    CHECK(scs_reason_get(cap_reject_req, sizeof(cap_reject_req),
                         SCS_REASON_MSGTYPE_REJECT_REQ, &r) == 1,
          "decoding a real REJECT_REQ failed");
    CHECK(r == 0, "a real VMS REJECT_REQ reads %u at the reason slot, expected 0."
                  " If this is a genuine observation, the placement in"
                  " scs_reason.h is WRONG and must be re-derived.", (unsigned)r);
    r = 0xbeef;
    CHECK(scs_reason_get(cap_disconnect_req, sizeof(cap_disconnect_req),
                         SCS_REASON_MSGTYPE_DISCONNECT_REQ, &r) == 1,
          "decoding a real DISCONNECT_REQ failed");
    CHECK(r == 0, "a real VMS DISCONNECT_REQ reads %u at the reason slot, expected 0",
          (unsigned)r);

    /* And the word the OVMX slot is NOT allowed to be: payload [60:62]. It is
     * an observed constant 0x0001 on REJECT_REQ but it VARIES on
     * DISCONNECT_REQ (CENSUS-A in scs_reason.h), i.e. a live field with an
     * undecoded meaning. Pinned so a future "just move it two bytes" cannot
     * land silently. */
    CHECK(cap_reject_req[74] == 0x01 && cap_reject_req[75] == 0x00,
          "the REJECT_REQ specimen no longer carries the observed 0x0001 at"
          " payload [60:62] -- the specimen was edited");
    CHECK(SCS_REASON_FRAME_OFF == 72u,
          "the reason-code slot moved to abs %u; if that is deliberate, the"
          " census in scs_reason.h and spec sec 5 must be redone first",
          (unsigned)SCS_REASON_FRAME_OFF);
}

/* (2) Byte position, LE order, round-trip, and keep-everything-else. */
static void test_byte_position_and_round_trip(void)
{
    static const uint16_t vals[] = {0, 1, 2, 5, 7, 0x00ff, 0x0100, 0x1234, 0x8000, 0xffff};

    for (unsigned m = 0; m < 2; m++) {
        unsigned msgtype = m == 0 ? SCS_REASON_MSGTYPE_REJECT_REQ
                                  : SCS_REASON_MSGTYPE_DISCONNECT_REQ;
        const uint8_t *src = m == 0 ? cap_reject_req : cap_disconnect_req;
        for (unsigned i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
            uint8_t frame[76];
            memcpy(frame, src, sizeof(frame));
            CHECK(scs_reason_put(frame, sizeof(frame), msgtype, vals[i]) == 1,
                  "put(%u) on msgtype %u did not write", (unsigned)vals[i], msgtype);
            CHECK(frame[SCS_REASON_FRAME_OFF] == (uint8_t)(vals[i] & 0xff),
                  "put(%u): low byte at abs %u is 0x%02x", (unsigned)vals[i],
                  (unsigned)SCS_REASON_FRAME_OFF, frame[SCS_REASON_FRAME_OFF]);
            CHECK(frame[SCS_REASON_FRAME_OFF + 1] == (uint8_t)(vals[i] >> 8),
                  "put(%u): high byte at abs %u is 0x%02x", (unsigned)vals[i],
                  (unsigned)SCS_REASON_FRAME_OFF + 1, frame[SCS_REASON_FRAME_OFF + 1]);

            uint16_t back = 0xbeef;
            CHECK(scs_reason_get(frame, sizeof(frame), msgtype, &back) == 1,
                  "get after put(%u) failed", (unsigned)vals[i]);
            CHECK(back == vals[i], "round-trip of %u came back %u",
                  (unsigned)vals[i], (unsigned)back);

            /* Keep-everything-else: exactly two bytes differ from the capture,
             * and they are the two the header names. */
            unsigned differing = 0;
            for (size_t b = 0; b < sizeof(frame); b++) {
                if (frame[b] != src[b]) {
                    differing++;
                    CHECK(b == SCS_REASON_FRAME_OFF || b == SCS_REASON_FRAME_OFF + 1,
                          "put(%u) changed byte %zu, which is not the reason slot",
                          (unsigned)vals[i], b);
                }
            }
            CHECK(differing <= 2, "put(%u) changed %u bytes", (unsigned)vals[i], differing);
        }
    }
}

/* (2) Refusals: the wrong message type, a short frame, NULL arguments. */
static void test_non_carriers_and_bad_arguments_are_refused(void)
{
    static const unsigned non_carriers[] = {0, 1, 2, 3, 5, 7, 8, 0x1234};
    uint8_t frame[76];
    uint16_t out = 0;

    for (unsigned i = 0; i < sizeof(non_carriers) / sizeof(non_carriers[0]); i++) {
        memcpy(frame, cap_reject_req, sizeof(frame));
        CHECK(scs_reason_carried_by(non_carriers[i]) == 0,
              "msgtype %u was called a reason-code carrier", non_carriers[i]);
        CHECK(scs_reason_put(frame, sizeof(frame), non_carriers[i], 3) == -1,
              "put accepted msgtype %u", non_carriers[i]);
        CHECK(memcmp(frame, cap_reject_req, sizeof(frame)) == 0,
              "a refused put on msgtype %u still changed the frame", non_carriers[i]);
        CHECK(scs_reason_get(frame, sizeof(frame), non_carriers[i], &out) == -1,
              "get accepted msgtype %u", non_carriers[i]);
    }

    /* 5 and 7 are REJECT_RSP and DISCONNECT_RSP if the type enum runs in figure
     * order -- and neither value has EVER appeared on our wire (spec sec
     * 4(h)(1a)). p. 2-26 gives the reason code to the two REQ frames only, so
     * they are refused here rather than speculatively supported. */

    memcpy(frame, cap_reject_req, sizeof(frame));
    CHECK(scs_reason_put(frame, SCS_REASON_FRAME_OFF + 1, SCS_REASON_MSGTYPE_REJECT_REQ, 1) == -1,
          "put accepted a frame one byte too short");
    CHECK(scs_reason_get(frame, SCS_REASON_FRAME_OFF + 1, SCS_REASON_MSGTYPE_REJECT_REQ,
                         &out) == -1,
          "get accepted a frame one byte too short");
    CHECK(scs_reason_put(NULL, sizeof(frame), SCS_REASON_MSGTYPE_REJECT_REQ, 1) == -1,
          "put accepted a NULL frame");
    CHECK(scs_reason_get(NULL, sizeof(frame), SCS_REASON_MSGTYPE_REJECT_REQ, &out) == -1,
          "get accepted a NULL frame");
    CHECK(scs_reason_get(frame, sizeof(frame), SCS_REASON_MSGTYPE_REJECT_REQ, NULL) == -1,
          "get accepted a NULL out pointer");
}

static void test_code_names(void)
{
    CHECK(strcmp(scs_reason_name(SCS_REASON_NONE), "NONE") == 0, "NONE");
    CHECK(strcmp(scs_reason_name(SCS_REASON_SYSAP_SHUTDOWN), "SYSAP_SHUTDOWN") == 0,
          "SYSAP_SHUTDOWN");
    CHECK(strcmp(scs_reason_name(SCS_REASON_PEER_DISCONNECT), "PEER_DISCONNECT") == 0,
          "PEER_DISCONNECT");
    /* A value OVMX did not define -- possibly a real VMS one. It must read as
     * UNKNOWN and never as a guess. */
    CHECK(strcmp(scs_reason_name(0x4321), "UNKNOWN") == 0,
          "an undefined code was given a name");
    CHECK(strcmp(scs_reason_name(8), "UNKNOWN") == 0,
          "the value one past the OVMX namespace was given a name");
}

/*
 * (3) THE KILL SWITCH, RUN -- guardrail 23.
 *
 * Bracketed on BOTH sides: the same put and the same get are performed with the
 * switch clear, then set, then clear again, and the difference is asserted. A
 * suppressed put is asserted to have changed NO BYTE of the frame, which is the
 * property that makes the switch a real off and not just a different return
 * code.
 */
static void test_kill_switch_suppresses_both_halves(void)
{
    uint8_t frame[76];
    uint16_t out;

    unsetenv("OVMX_NO_REASON_CODE");
    CHECK(scs_reason_enabled() == 1, "control: the field is off with the switch clear");
    memcpy(frame, cap_disconnect_req, sizeof(frame));
    CHECK(scs_reason_put(frame, sizeof(frame), SCS_REASON_MSGTYPE_DISCONNECT_REQ,
                         SCS_REASON_SYSAP_SHUTDOWN) == 1,
          "control: the enabled put did not write");
    CHECK(frame[SCS_REASON_FRAME_OFF] == SCS_REASON_SYSAP_SHUTDOWN,
          "control: the enabled put wrote 0x%02x", frame[SCS_REASON_FRAME_OFF]);
    out = 0xbeef;
    CHECK(scs_reason_get(frame, sizeof(frame), SCS_REASON_MSGTYPE_DISCONNECT_REQ, &out) == 1 &&
          out == SCS_REASON_SYSAP_SHUTDOWN,
          "control: the enabled get returned %u", (unsigned)out);

    setenv("OVMX_NO_REASON_CODE", "1", 1);
    CHECK(scs_reason_enabled() == 0, "the switch did not disable the field");
    memcpy(frame, cap_disconnect_req, sizeof(frame));
    CHECK(scs_reason_put(frame, sizeof(frame), SCS_REASON_MSGTYPE_DISCONNECT_REQ,
                         SCS_REASON_SYSAP_SHUTDOWN) == 0,
          "the suppressed put did not report suppression");
    CHECK(memcmp(frame, cap_disconnect_req, sizeof(frame)) == 0,
          "THE KILL SWITCH DOES NOT GATE THE WRITE: a suppressed put changed the frame");
    out = 0xbeef;
    CHECK(scs_reason_get(cap_disconnect_req, sizeof(cap_disconnect_req),
                         SCS_REASON_MSGTYPE_DISCONNECT_REQ, &out) == 0,
          "the suppressed get did not report suppression");
    CHECK(out == 0xbeef, "the suppressed get wrote %u into the caller's variable",
          (unsigned)out);

    /* "0" and the empty string are NOT set. */
    setenv("OVMX_NO_REASON_CODE", "0", 1);
    CHECK(scs_reason_enabled() == 1, "OVMX_NO_REASON_CODE=0 disabled the field");
    setenv("OVMX_NO_REASON_CODE", "", 1);
    CHECK(scs_reason_enabled() == 1, "an empty OVMX_NO_REASON_CODE disabled the field");

    unsetenv("OVMX_NO_REASON_CODE");
    CHECK(scs_reason_enabled() == 1, "the bracketing control did not come back on");
    memcpy(frame, cap_disconnect_req, sizeof(frame));
    CHECK(scs_reason_put(frame, sizeof(frame), SCS_REASON_MSGTYPE_DISCONNECT_REQ,
                         SCS_REASON_SYSAP_SHUTDOWN) == 1,
          "the bracketing control put did not write");
}

int main(void)
{
    test_the_captured_frames_are_the_two_carriers();
    test_real_vms_frames_read_zero_at_the_slot();
    test_byte_position_and_round_trip();
    test_non_carriers_and_bad_arguments_are_refused();
    test_code_names();
    test_kill_switch_suppresses_both_halves();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
