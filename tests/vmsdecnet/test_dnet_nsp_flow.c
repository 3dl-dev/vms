/*
 * test_dnet_nsp_flow.c - NSP data flow: receive window, link-service flow
 * control, acknowledgement / NAK and retransmission (rd vms-985).
 *
 * The vms-0a7 S1 live bench against a real OpenVMS VAX V7.3
 * (tests/lab/captures/vms-0a7-s1-20261102/, README "The two large-file
 * failures, on the wire") found two faults in OVMX's NSP:
 *
 *   VAX -> OVMX (link OVMXA 0xc935 <-> VAXC 0x2008, wire.txt 18.540-18.852):
 *     the VAX sends segment after segment (FCOPT none); OVMX acked each one as
 *     it arrived, queued it for the client, and when the 32-entry queue was
 *     full sent Disconnect Initiate reason 1. The client saw -RMS-F-RER.
 *
 *   OVMX -> VAX (link OVMXA 0xc961 <-> VAXC 0x204b, wire.txt 227.016-227.021):
 *     the VAX stopped OVMX with link service `00 80 02 00 01 00` (other-data 2,
 *     LSFLAGS FCMOD 1), resumed it with `00 80 03 00 02 00`, and NAKed 0x900e.
 *     OVMX's decoder read the SEGNUM byte as LSFLAGS, so it never stopped, and
 *     it kept no copy of what it sent, so it never retransmitted: the COPY hung.
 *
 * Each test below replays the real VAX PDUs from that capture (the NSP bytes
 * the bench decoder printed, with the link addresses of that link) into the
 * link FSM and asserts what OVMX now does. The two-link transfers then move
 * 20000 records each way through a channel that loses frames, with a reader
 * that falls behind, and require every record to arrive once, in order,
 * byte-identical, with no Disconnect Initiate -- the done-criterion of the
 * live run, here on the FSM.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dnet_link.h"
#include "dnet_nsp.h"

static int g_fail = 0;
static int g_pass = 0;
#define CHECK(c, msg) do { \
    if (c) g_pass++; \
    else { g_fail++; printf("  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

static int decode_hex(const uint8_t *b, size_t n, struct dnet_nsp_msg *m)
{
    return dnet_nsp_decode(b, n, m, NULL);
}

static int encode_eq(const struct dnet_nsp_msg *m, const uint8_t *want, size_t wn)
{
    uint8_t buf[DNET_NSP_MAX_DATA + 32];
    size_t n = 0;
    if (dnet_nsp_encode(m, buf, sizeof buf, &n) != DNET_NSP_OK)
        return 0;
    return n == wn && memcmp(buf, want, wn) == 0;
}

/* Bring `lk` (local `la`) to RUN as the initiator against a VAX CC from `ra`. */
static void up_initiator(struct dnet_link *lk, uint16_t la, uint16_t ra)
{
    struct dnet_nsp_msg ci, cc;
    dnet_link_init(lk, la, (1u << 10) | 965u, 100);
    dnet_link_connect(lk, NULL, 0, 1459, 0x01, DNET_NSP_VER_41, &ci, 100);
    /* The VAX's CC: services 0x01, info 0x03, segsize 1459, data 00. */
    const uint8_t ccb[] = { 0x28, (uint8_t)la, (uint8_t)(la >> 8), (uint8_t)ra,
                            (uint8_t)(ra >> 8), 0x01, 0x03, 0xb3, 0x05, 0x00 };
    decode_hex(ccb, sizeof ccb, &cc);
    dnet_link_rx(lk, &cc, 100, NULL, NULL, NULL);
}

/* A VAX data segment to `la` from `ra`: ACKNUM `ack`, SEGNUM `seg`, payload. */
static void vax_data(struct dnet_nsp_msg *m, uint16_t la, uint16_t ra, uint16_t ack,
                     uint16_t seg, const char *payload)
{
    memset(m, 0, sizeof *m);
    m->type = DNET_NSP_T_DATA;
    m->msgflg = DNET_NSP_MSGFLG_DATA;
    m->dstaddr = la; m->srcaddr = ra;
    m->has_acknum = 1; m->acknum = ack;
    m->segnum = seg;
    m->datalen = (uint16_t)strlen(payload);
    memcpy(m->data, payload, m->datalen);
}

/* ------------------------------------------------------------------------ */
/* VAX -> OVMX: the 0g link. A slow reader must stop the VAX, NAK what does
 * not fit, resume it, and never disconnect. */
static void test_vax_to_ovmx_replay(void)
{
    printf("[rx-window] VAX->OVMX replay of the 0g link (0xc935 <-> 0x2008)\n");
    const uint16_t LA = 0xc935, RA = 0x2008;
    struct dnet_link A;
    up_initiator(&A, LA, RA);
    CHECK(dnet_link_is_up(&A), "OVMX link RUN after the VAX's CC");
    dnet_link_rx_window(&A, 32);           /* NETACP_OUT_RXQ */

    struct dnet_nsp_msg out, rep, in;
    int has = 0;
    enum dnet_link_event ev;

    /* OVMX's post-CC link service: other-data 1, LSFLAGS 0, FCVAL 0. */
    CHECK(dnet_link_link_service(&A, &out, 100) == DNET_LINK_OK, "post-CC LS built");
    const uint8_t ls1[] = { 0x10, 0x08, 0x20, 0x35, 0xc9, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00 };
    CHECK(encode_eq(&out, ls1, sizeof ls1),
          "post-CC LS is the VAX's own form: 10 08 20 35 c9 00 80 01 00 00 00");

    /* The VAX's link service, wire.txt 18.543: `00 80 01 00 00 00`. */
    const uint8_t vls[] = { 0x10, 0x35, 0xc9, 0x08, 0x20, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00 };
    CHECK(decode_hex(vls, sizeof vls, &in) == DNET_NSP_OK && in.segnum == 1 &&
          in.ls_flags == 0 && in.fc_val == 0, "VAX LS decodes: other-data 1, LSFLAGS 0");
    dnet_link_rx(&A, &in, 100, &rep, &has, &ev);
    const uint8_t oack1[] = { 0x14, 0x08, 0x20, 0x35, 0xc9, 0x01, 0x80 };
    CHECK(has && encode_eq(&rep, oack1, sizeof oack1), "OVMX other-acks it: 14 .. 01 80");
    /* The VAX's other-data ack of our LS (wire: our LS was never acked because
     * it was malformed; now it is well-formed and the VAX acks it). */
    const uint8_t vack[] = { 0x14, 0x35, 0xc9, 0x08, 0x20, 0x01, 0x80 };
    decode_hex(vack, sizeof vack, &in);
    dnet_link_rx(&A, &in, 100, NULL, NULL, NULL);
    CHECK(!A.ls_out, "our post-CC LS acknowledged");

    /* The VAX streams segments 1..40 (the S1 run broke at 0x25 = 37) while the
     * reader takes nothing. Real header shape: `05 80 <seg> 10|00 ...`. */
    unsigned delivered = 0, naks = 0, held = 0, stops = 0, starts = 0, dis = 0;
    char p[64];
    for (uint16_t seg = 1; seg <= 40; seg++) {
        snprintf(p, sizeof p, "record %04u of the S1 big file", (unsigned)seg);
        vax_data(&in, LA, RA, 0x8005, (uint16_t)(seg | ((seg & 1) ? 0x1000 : 0)), p);
        has = 0; ev = DNET_LINK_EV_NONE;
        dnet_link_rx(&A, &in, 101, &rep, &has, &ev);
        CHECK(has && rep.type == DNET_NSP_T_ACK, "every VAX segment is answered");
        if (ev == DNET_LINK_EV_DATA) {
            delivered++;
            CHECK(rep.acknum == (uint16_t)(0x8000 | seg), "an accepted segment is ACKed by number");
        } else if ((rep.acknum & 0x7000) == 0x1000) {
            naks++;
            CHECK(seg == 33 && (rep.acknum & 0x0fff) == 32,
                  "the first segment with no room is NAKed at the last one buffered (0x9020)");
        } else {
            held++;
            CHECK(seg > 33 && rep.acknum == 0x8020,
                  "the ones after it are acked at 32 only (one NAK per gap, no NAK storm)");
        }
        for (;;) {
            int h = 0;
            dnet_link_poll(&A, 101, &out, &h, &ev);
            if (!h) break;
            if (out.type == DNET_NSP_T_DI) dis++;
            if (out.type == DNET_NSP_T_LS) {
                if ((out.ls_flags & 3) == DNET_NSP_LS_FCMOD_STOP) stops++;
                if ((out.ls_flags & 3) == DNET_NSP_LS_FCMOD_START) starts++;
            }
            if (out.type == DNET_NSP_T_LS && (out.ls_flags & 3) == DNET_NSP_LS_FCMOD_STOP) {
                /* Byte form of the stop: other-data 2, FCMOD 1, FCVAL 0, acking
                 * the VAX's other-data 1 -- the mirror of the VAX's own stop. */
                const uint8_t stop[] = { 0x10, 0x08, 0x20, 0x35, 0xc9, 0x01, 0x80,
                                         0x02, 0x00, 0x01, 0x00 };
                CHECK(encode_eq(&out, stop, sizeof stop),
                      "stop LS is 10 08 20 35 c9 01 80 02 00 01 00");
                CHECK(seg == 16, "the stop goes when half the window is used");
            }
        }
    }
    CHECK(delivered == 32 && naks == 1 && held == 7,
          "32 segments buffered; of the 8 with no room, none acknowledged, the first NAKed");
    CHECK(stops == 1 && starts == 0 && dis == 0,
          "one stop, no resume yet, and NO Disconnect Initiate (was reason 1)");
    CHECK(dnet_link_is_up(&A), "the link is still RUN with the reader 40 segments behind");

    /* The VAX other-acks the stop. The reader drains: no resume until 24 free. */
    const uint8_t vack2[] = { 0x14, 0x35, 0xc9, 0x08, 0x20, 0x02, 0x80 };
    decode_hex(vack2, sizeof vack2, &in);
    dnet_link_rx(&A, &in, 102, NULL, NULL, NULL);
    dnet_link_rx_room(&A, 20);
    dnet_link_poll(&A, 102, &out, &has, NULL);
    CHECK(!has, "no resume at 20 free (resume mark 24)");
    dnet_link_rx_room(&A, 24);
    dnet_link_poll(&A, 102, &out, &has, NULL);
    const uint8_t start[] = { 0x10, 0x08, 0x20, 0x35, 0xc9, 0x01, 0x80, 0x03, 0x00, 0x02, 0x00 };
    CHECK(has && encode_eq(&out, start, sizeof start),
          "resume LS is 10 08 20 35 c9 01 80 03 00 02 00 (the VAX's own resume shape)");

    /* The VAX goes back to 33 (after our NAK 32) and it is taken in order. */
    vax_data(&in, LA, RA, 0x8005, 33, "record 0033 of the S1 big file");
    dnet_link_rx(&A, &in, 103, &rep, &has, &ev);
    CHECK(ev == DNET_LINK_EV_DATA && rep.acknum == 0x8021, "the resent 33 is accepted and ACKed");
    /* A duplicate (our ACK lost) is re-ACKed, not delivered twice. */
    dnet_link_rx(&A, &in, 103, &rep, &has, &ev);
    CHECK(ev == DNET_LINK_EV_NONE && has && rep.acknum == 0x8021,
          "a duplicate 33 is re-ACKed and not delivered again");
}

/* ------------------------------------------------------------------------ */
/* OVMX -> VAX: the 7c link. Honour stop / resume, retransmit after the NAK. */
static void test_ovmx_to_vax_replay(void)
{
    printf("[tx-flow] OVMX->VAX replay of the 7c link (0xc961 <-> 0x204b)\n");
    const uint16_t LA = 0xc961, RA = 0x204b;
    struct dnet_link A;
    up_initiator(&A, LA, RA);
    CHECK(A.rem_fcopt == DNET_NSP_SVC_FCOPT_NONE, "the VAX's CC offers FCOPT none");

    struct dnet_nsp_msg out, in;
    struct dnet_nsp_msg wire[64];       /* every data PDU we put on the wire */
    unsigned nwire = 0;
    char rec[64];
    int has = 0;
    enum dnet_link_event ev;

    /* Records 1..8 go straight out; the 9th waits for an ACK (queue of 8). */
    for (unsigned r = 1; r <= 8; r++) {
        snprintf(rec, sizeof rec, "record %04u of the S1 file", r);
        CHECK(dnet_link_send_data(&A, (const uint8_t *)rec, strlen(rec), &out, 200) ==
              DNET_LINK_OK && out.segnum == r, "record goes out with the next segment number");
        CHECK(out.has_acknum && out.acknum == 0x8000, "data carries ack 0, as a VAX's does");
        wire[nwire++] = out;
    }
    snprintf(rec, sizeof rec, "record %04u of the S1 file", 9u);
    CHECK(dnet_link_send_data(&A, (const uint8_t *)rec, strlen(rec), &out, 200) ==
          DNET_LINK_EAGAIN, "a 9th unacknowledged record is refused (EAGAIN), not dropped");

    /* The VAX ACKs through 5: 04 61 c9 4b 20 05 80. */
    const uint8_t ack5[] = { 0x04, 0x61, 0xc9, 0x4b, 0x20, 0x05, 0x80 };
    decode_hex(ack5, sizeof ack5, &in);
    dnet_link_rx(&A, &in, 200, NULL, NULL, NULL);
    CHECK(dnet_link_tx_room(&A) == 5, "ACK 5 frees five queue entries");
    for (unsigned r = 9; r <= 13; r++) {
        snprintf(rec, sizeof rec, "record %04u of the S1 file", r);
        CHECK(dnet_link_send_data(&A, (const uint8_t *)rec, strlen(rec), &out, 200) ==
              DNET_LINK_OK, "records 9..13 go out");
        wire[nwire++] = out;
    }

    /* 227.016: the VAX's stop, `00 80 02 00 01 00` (other-data 2... after its
     * post-CC LS 1). Feed its post-CC LS first so other-data numbering matches. */
    const uint8_t vls1[] = { 0x10, 0x61, 0xc9, 0x4b, 0x20, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00 };
    decode_hex(vls1, sizeof vls1, &in);
    dnet_link_rx(&A, &in, 200, &out, &has, &ev);
    const uint8_t vstop[] = { 0x10, 0x61, 0xc9, 0x4b, 0x20, 0x00, 0x80, 0x02, 0x00, 0x01, 0x00 };
    CHECK(decode_hex(vstop, sizeof vstop, &in) == DNET_NSP_OK && in.segnum == 2 &&
          in.ls_flags == DNET_NSP_LS_FCMOD_STOP,
          "the VAX's 00 80 02 00 01 00 decodes as other-data 2, FCMOD stop (was read as LSFLAGS 2)");
    dnet_link_rx(&A, &in, 200, &out, &has, &ev);
    const uint8_t oack2[] = { 0x14, 0x4b, 0x20, 0x61, 0xc9, 0x02, 0x80 };
    CHECK(has && encode_eq(&out, oack2, sizeof oack2) && ev == DNET_LINK_EV_LINK_SERVICE,
          "OVMX other-acks the stop (14 .. 02 80)");
    CHECK(A.rem_stopped, "OVMX is stopped");

    /* ACK through 13 frees room; record 14 queues but must NOT go out. */
    const uint8_t ack13[] = { 0x04, 0x61, 0xc9, 0x4b, 0x20, 0x0d, 0x80 };
    decode_hex(ack13, sizeof ack13, &in);
    dnet_link_rx(&A, &in, 201, NULL, NULL, NULL);
    for (unsigned r = 14; r <= 17; r++) {
        snprintf(rec, sizeof rec, "record %04u of the S1 file", r);
        CHECK(dnet_link_send_data(&A, (const uint8_t *)rec, strlen(rec), &out, 201) ==
              DNET_LINK_QUEUED, "while stopped a record is queued, not sent");
    }
    dnet_link_poll(&A, 201 + 100, &out, &has, &ev);
    CHECK(!has && ev == DNET_LINK_EV_NONE && dnet_link_is_up(&A),
          "while stopped nothing goes out and no timer gives the link up (100 s later)");

    /* 227.021: the VAX's resume, `00 80 03 00 02 00`. */
    const uint8_t vgo[] = { 0x10, 0x61, 0xc9, 0x4b, 0x20, 0x00, 0x80, 0x03, 0x00, 0x02, 0x00 };
    decode_hex(vgo, sizeof vgo, &in);
    dnet_link_rx(&A, &in, 301, &out, &has, &ev);
    CHECK(!A.rem_stopped, "resume clears the stop");
    unsigned sent = 0;
    for (;;) {
        dnet_link_poll(&A, 301, &out, &has, NULL);
        if (!has) break;
        CHECK(out.type == DNET_NSP_T_DATA && out.segnum == 14 + sent,
              "held records go out in order 14, 15, 16, 17");
        wire[nwire++] = out;
        sent++;
    }
    CHECK(sent == 4, "all four held records sent after the resume");

    /* The VAX's NAK 0x900e: it has 14, wants 15 onward again. */
    const uint8_t nak14[] = { 0x04, 0x61, 0xc9, 0x4b, 0x20, 0x0e, 0x90 };
    decode_hex(nak14, sizeof nak14, &in);
    dnet_link_rx(&A, &in, 302, NULL, NULL, NULL);
    unsigned rtx = 0;
    for (;;) {
        dnet_link_poll(&A, 302, &out, &has, NULL);
        if (!has) break;
        uint16_t want = (uint16_t)(15 + rtx);
        const struct dnet_nsp_msg *orig = NULL;
        for (unsigned k = 0; k < nwire; k++)
            if (wire[k].segnum == want) orig = &wire[k];
        CHECK(orig && out.segnum == want && out.datalen == orig->datalen &&
              memcmp(out.data, orig->data, out.datalen) == 0,
              "after NAK 14 OVMX resends 15.. byte-identical, in order");
        rtx++;
    }
    CHECK(rtx == 3, "15, 16, 17 retransmitted after the NAK (nothing before 15)");

    /* No ACK at all now: the retransmit timer resends from 15, and after the
     * RETRANSMIT FACTOR the link is declared dead (reason 38), never silently. */
    dnet_tick_t t = 302;
    unsigned rounds = 0, gone = 0;
    for (unsigned guard = 0; guard < 200 && !gone; guard++) {
        t += DNET_LINK_RTX_SECS;
        for (;;) {
            dnet_link_poll(&A, t, &out, &has, &ev);
            if (ev == DNET_LINK_EV_DISCONNECT) { gone = 1; break; }
            if (!has) break;
            if (out.segnum == 15) rounds++;
        }
    }
    CHECK(rounds == DNET_LINK_RTX_MAX, "the timer resends from 15 RETRANSMIT FACTOR times");
    CHECK(gone && dnet_link_state_of(&A) == DNET_LINK_CLOSED &&
          A.disc_reason == DNET_LINK_REASON_UNREACHABLE,
          "then the link closes, reason 38, reported as a disconnect");
}

/* ------------------------------------------------------------------------ */
/* A normal close while the VAX holds us stopped: the DI waits for the tail. */
static void test_close_drains_before_di(void)
{
    printf("[close] the VAX stops OVMX, OVMX closes, the VAX resumes: all data before the DI\n");
    const uint16_t LA = 0xc961, RA = 0x204b;
    struct dnet_link A;
    up_initiator(&A, LA, RA);
    struct dnet_nsp_msg out, in;
    int has = 0;
    enum dnet_link_event ev;
    char rec[48];

    /* Records 1..3 go out; the VAX acks 1 and stops us (its post-CC LS, then
     * the 227.016 stop). */
    for (unsigned r = 1; r <= 3; r++) {
        snprintf(rec, sizeof rec, "record %04u", r);
        dnet_link_send_data(&A, (const uint8_t *)rec, strlen(rec), &out, 10);
    }
    const uint8_t ack1[] = { 0x04, 0x61, 0xc9, 0x4b, 0x20, 0x01, 0x80 };
    decode_hex(ack1, sizeof ack1, &in);
    dnet_link_rx(&A, &in, 10, NULL, NULL, NULL);
    const uint8_t vls1[] = { 0x10, 0x61, 0xc9, 0x4b, 0x20, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00 };
    const uint8_t vstop[] = { 0x10, 0x61, 0xc9, 0x4b, 0x20, 0x00, 0x80, 0x02, 0x00, 0x01, 0x00 };
    decode_hex(vls1, sizeof vls1, &in);  dnet_link_rx(&A, &in, 10, &out, &has, &ev);
    decode_hex(vstop, sizeof vstop, &in); dnet_link_rx(&A, &in, 10, &out, &has, &ev);
    /* While stopped: records 4..6 queue, and the client closes. */
    for (unsigned r = 4; r <= 6; r++) {
        snprintf(rec, sizeof rec, "record %04u", r);
        CHECK(dnet_link_send_data(&A, (const uint8_t *)rec, strlen(rec), &out, 11) ==
              DNET_LINK_QUEUED, "records queue while stopped");
    }
    CHECK(dnet_link_close(&A, DNET_LINK_REASON_NORMAL, &out, 11) == DNET_LINK_QUEUED,
          "close with data outstanding is queued -- no DI yet");
    CHECK(dnet_link_send_data(&A, (const uint8_t *)"x", 1, &out, 11) == DNET_LINK_ESTATE,
          "a closing link takes no more data");
    for (dnet_tick_t t = 11; t < 60; t++) {
        dnet_link_poll(&A, t, &out, &has, &ev);
        CHECK(!has && dnet_link_is_up(&A), "stopped: no data and no DI goes out");
    }

    /* The VAX resumes. 2 and 3 are already on the wire; the held 4..6 go in
     * order, the VAX acks what it has, and only after the ACK of 6 does the
     * DI go. */
    const uint8_t vgo[] = { 0x10, 0x61, 0xc9, 0x4b, 0x20, 0x00, 0x80, 0x03, 0x00, 0x02, 0x00 };
    decode_hex(vgo, sizeof vgo, &in); dnet_link_rx(&A, &in, 60, &out, &has, &ev);
    unsigned next = 4, di = 0, data_after_di = 0;
    for (int guard = 0; guard < 100 && !di; guard++) {
        dnet_link_poll(&A, 60, &out, &has, &ev);
        if (!has) {
            /* The VAX acks what it got so far. */
            uint8_t a[] = { 0x04, 0x61, 0xc9, 0x4b, 0x20, (uint8_t)(next - 1), 0x80 };
            decode_hex(a, sizeof a, &in);
            dnet_link_rx(&A, &in, 60, NULL, NULL, NULL);
            continue;
        }
        if (out.type == DNET_NSP_T_DI) {
            di = 1;
            CHECK(next == 7 && A.tx_count == 0, "the DI follows the ACK of the last record (6)");
            CHECK(out.reason == DNET_LINK_REASON_NORMAL, "DI reason normal");
        } else if (out.type == DNET_NSP_T_DATA) {
            if (di) data_after_di++;
            snprintf(rec, sizeof rec, "record %04u", next);
            CHECK(out.segnum == next && out.datalen == strlen(rec) &&
                  memcmp(out.data, rec, out.datalen) == 0, "the tail goes in order, byte-identical");
            next++;
        }
    }
    CHECK(di && data_after_di == 0 && dnet_link_state_of(&A) == DNET_LINK_DI_SENT,
          "all six records arrived before the DI; the link is DI_SENT");

    /* With nothing outstanding a close is immediate. */
    struct dnet_link B;
    up_initiator(&B, LA, RA);
    CHECK(dnet_link_close(&B, DNET_LINK_REASON_NORMAL, &out, 5) == DNET_LINK_OK &&
          out.type == DNET_NSP_T_DI, "an idle link closes at once");
}

/* ------------------------------------------------------------------------ */
/* Two links, 20000 records, lossy channel, slow reader. */

struct chan {
    struct dnet_nsp_msg q[256];
    unsigned head, count;
    uint64_t rng;
    unsigned drop_per_1000;
    unsigned long dropped;
};

static uint64_t sm64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Put a PDU on the wire through the codec (bytes, then decode), maybe lose it. */
static void chan_put(struct chan *c, const struct dnet_nsp_msg *m)
{
    uint8_t b[DNET_NSP_MAX_DATA + 32];
    size_t n = 0;
    if (dnet_nsp_encode(m, b, sizeof b, &n) != DNET_NSP_OK) { g_fail++; return; }
    if ((sm64(&c->rng) % 1000u) < c->drop_per_1000) { c->dropped++; return; }
    if (c->count == 256) { c->dropped++; return; }   /* a full wire loses frames */
    struct dnet_nsp_msg *d = &c->q[(c->head + c->count) % 256];
    if (dnet_nsp_decode(b, n, d, NULL) != DNET_NSP_OK) { g_fail++; return; }
    c->count++;
}

static int chan_get(struct chan *c, struct dnet_nsp_msg *m)
{
    if (!c->count) return 0;
    *m = c->q[c->head];
    c->head = (c->head + 1) % 256;
    c->count--;
    return 1;
}

static void flush(struct dnet_link *lk, struct chan *to, dnet_tick_t now, unsigned *dis)
{
    struct dnet_nsp_msg out;
    int has;
    enum dnet_link_event ev;
    for (;;) {
        dnet_link_poll(lk, now, &out, &has, &ev);
        if (ev == DNET_LINK_EV_DISCONNECT) (*dis)++;
        if (!has) break;
        if (out.type == DNET_NSP_T_DI) (*dis)++;
        chan_put(to, &out);
    }
}

#define BIG_RECORDS 20000u
#define RXCAP 32u

static void transfer(const char *tag, int initiator_sends, uint64_t seed)
{
    printf("[transfer] %s: %u records, %u/1000 frames lost, slow reader\n", tag,
           BIG_RECORDS, 20u);
    struct dnet_link I, R;                       /* initiator, responder */
    struct dnet_nsp_msg m, rep;
    struct chan i2r, r2i;
    memset(&i2r, 0, sizeof i2r); memset(&r2i, 0, sizeof r2i);
    i2r.rng = seed; r2i.rng = seed ^ 0x55u;

    dnet_link_init(&I, 0xc961, 1, 1);
    dnet_link_init(&R, 0x204b, 2, 1);
    dnet_link_connect(&I, NULL, 0, 1459, 0x01, DNET_NSP_VER_41, &m, 1);
    int has = 0; enum dnet_link_event ev;
    dnet_link_rx(&R, &m, 1, &rep, &has, &ev);
    dnet_link_accept(&R, &m, 1);
    dnet_link_rx(&I, &m, 1, &rep, &has, &ev);
    CHECK(dnet_link_is_up(&I) && dnet_link_is_up(&R), "both ends RUN");
    i2r.drop_per_1000 = r2i.drop_per_1000 = 20;

    struct dnet_link *S = initiator_sends ? &I : &R;
    struct dnet_link *D = initiator_sends ? &R : &I;
    struct chan *s2d = initiator_sends ? &i2r : &r2i;
    struct chan *d2s = initiator_sends ? &r2i : &i2r;
    dnet_link_rx_window(D, RXCAP);

    /* The reader's buffer (what NETACP's rxq is). */
    static char rq[RXCAP][64];
    unsigned rq_head = 0, rq_n = 0;
    unsigned next_send = 1, next_read = 1, dis = 0, bad = 0;
    dnet_tick_t now = 2;
    char rec[64];
    unsigned long iter = 0;

    while (next_read <= BIG_RECORDS && iter < 4000000ul) {
        iter++;
        /* Sender offers records while the link takes them. */
        while (next_send <= BIG_RECORDS) {
            int n = snprintf(rec, sizeof rec, "record %05u of LONG.TXT", next_send);
            int rc = dnet_link_send_data(S, (const uint8_t *)rec, (size_t)n, &m, now);
            if (rc == DNET_LINK_EAGAIN) break;
            if (rc == DNET_LINK_OK) chan_put(s2d, &m);
            else if (rc != DNET_LINK_QUEUED) { bad++; break; }
            next_send++;
        }
        flush(S, s2d, now, &dis);
        /* Deliver a few frames each way. */
        for (int k = 0; k < 4 && chan_get(s2d, &m); k++) {
            dnet_link_rx(D, &m, now, &rep, &has, &ev);
            if (has) chan_put(d2s, &rep);
            if (ev == DNET_LINK_EV_DATA) {
                if (rq_n < RXCAP) {
                    memcpy(rq[(rq_head + rq_n) % RXCAP], m.data, m.datalen);
                    rq[(rq_head + rq_n) % RXCAP][m.datalen] = 0;
                    rq_n++;
                } else {
                    bad++;                 /* the window let one in with no room */
                }
                dnet_link_rx_room(D, RXCAP - rq_n);
            }
            flush(D, d2s, now, &dis);
        }
        for (int k = 0; k < 4 && chan_get(d2s, &m); k++) {
            dnet_link_rx(S, &m, now, &rep, &has, &ev);
            if (has) chan_put(s2d, &rep);
        }
        flush(S, s2d, now, &dis);
        /* The reader is slower than the wire: one record every third round. */
        if (rq_n && (iter % 3u) == 0) {
            char want[64];
            snprintf(want, sizeof want, "record %05u of LONG.TXT", next_read);
            if (strcmp(rq[rq_head], want) != 0) bad++;
            rq_head = (rq_head + 1) % RXCAP;
            rq_n--;
            next_read++;
            dnet_link_rx_room(D, RXCAP - rq_n);
            flush(D, d2s, now, &dis);
        }
        /* Time moves when the wire is idle, so retransmit timers fire. */
        if (!s2d->count && !d2s->count)
            now++;
    }
    printf("  read %u records, sent %lu data PDUs (%lu retransmitted), %lu NAKs sent,"
           " %lu stop/resume LS, %lu frames lost\n", next_read - 1, S->data_sent,
           S->data_rtx, D->naks_sent, D->ls_sent, i2r.dropped + r2i.dropped);
    CHECK(next_read == BIG_RECORDS + 1, "every record read");
    CHECK(bad == 0, "in order, byte-identical, and never one past the window");
    CHECK(dis == 0 && dnet_link_is_up(S) && dnet_link_is_up(D),
          "no Disconnect Initiate and both links still RUN");
    CHECK(S->send_seq == (BIG_RECORDS & DNET_LINK_SEQ_MASK),
          "segment numbers ran modulo 4096 (wrapped through 0)");
    CHECK(S->data_rtx > 0 && D->ls_sent > 1, "losses were recovered by retransmission"
          " and the reader stopped/resumed the sender");
}

int main(void)
{
    test_vax_to_ovmx_replay();
    test_ovmx_to_vax_replay();
    test_close_drains_before_di();
    transfer("OVMX->VAX direction (initiator sends)", 1, 0x0a7u);
    transfer("VAX->OVMX direction (responder sends)", 0, 0x985u);
    printf("test_dnet_nsp_flow: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
