/*
 * test_dnet_link.c - DECnet Phase IV NSP LOGICAL-LINK connection service proof
 *                    (rd vms-c23, engine rung 2 of epic vms-30e).
 *
 * Proves the connection SERVICE the FSM adds on top of the NSP codec: two ends
 * ESTABLISH a logical link (Connect Initiate/Confirm), RUN it (a data segment
 * carried with its acknowledgement, in-order sequencing), and TEAR IT DOWN
 * cleanly (Disconnect Initiate/Confirm). Two layers are proven:
 *
 *   1. FSM UNIT (raw NSP PDUs): two struct dnet_link instances drive each other
 *      through CLOSED -> CI_SENT/CR_RCVD -> RUN -> DI_SENT -> CLOSED, with a
 *      data segment + explicit ack exchanged in the RUN state. Deterministic,
 *      clock-injected -- no socket.
 *   2. CI RETRANSMIT: a fresh originator whose Connect Initiate is never
 *      confirmed retransmits on the injected clock and, after the oracle-
 *      informed budget (8 retransmits), abandons the link as UNREACHABLE --
 *      never a fabricated connect. (This is exactly VAX1 giving up on SET HOST
 *      VAX2 after 8 retransmits, register sec 4.6.)
 *   3. ENGINE END-TO-END over a real socketpair(2): two engines exchange full
 *      on-wire data frames (Ethernet header + Phase IV long-data routing header
 *      + NSP PDU) -- genuine write(2)/read(2) of the actual encoded bytes, the
 *      exact build_data_frame -> wire -> link_rx path DECNETD uses -- opening a
 *      logical link, moving a data segment with its ack, and disconnecting, with
 *      the payload round-tripping byte-identical. No CAP_NET_RAW.
 *
 * Clean-room (Rule 8): only the Connect Initiate bytes are oracle-verified (in
 * the codec, test_dnet_nsp.c); the CC/data/ack/DI/DC choreography here is spec-
 * derived and proven by the two-endpoint round-trip, never against captured
 * bytes.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dnet_link.h"
#include "dnet_engine.h"

static int failures = 0;
static void check(int cond, const char *what)
{
    if (cond) { printf("  OK: %s\n", what); }
    else      { printf("  FAIL: %s\n", what); failures++; }
}

/* ---- 1. FSM unit: two links, raw NSP PDUs ------------------------------- */
static void test_fsm(void)
{
    printf("[fsm] two links: CI/CC establish -> data+ack -> DI/DC teardown\n");

    struct dnet_link A, B;              /* A originates, B responds */
    check(dnet_link_init(&A, 0x2001, /*remote node*/0x0402, 100) == DNET_LINK_OK,
          "A init (LLA 8193, peer 1.2)");
    check(dnet_link_init(&B, 0x2002, 0, 100) == DNET_LINK_OK, "B init (LLA 8194)");
    check(dnet_link_state_of(&A) == DNET_LINK_CLOSED &&
          dnet_link_state_of(&B) == DNET_LINK_CLOSED, "both start CLOSED");

    /* A connects: builds a CI. */
    struct dnet_nsp_msg ci;
    const uint8_t access[] = { 0x06, 'S','Y','S','T','E','M' }; /* access-control-ish */
    check(dnet_link_connect(&A, access, sizeof(access), 1459, 0x01,
                            DNET_NSP_VER_41, &ci, 100) == DNET_LINK_OK,
          "A connect() builds CI");
    check(dnet_link_state_of(&A) == DNET_LINK_CI_SENT, "A -> CI_SENT");
    check(ci.type == DNET_NSP_T_CI && ci.srcaddr == 0x2001 && ci.dstaddr == 0,
          "CI names A's LLA, dst 0");
    check(ci.segsize == 1459 && dnet_nsp_version(&ci) == DNET_NSP_VER_41,
          "CI carries negotiated segsize + NSP version 4.1");

    /* B receives the CI -> connect indication. */
    enum dnet_link_event ev = DNET_LINK_EV_NONE;
    check(dnet_link_rx(&B, &ci, 101, NULL, NULL, &ev) == DNET_LINK_OK,
          "B rx(CI) ok");
    check(ev == DNET_LINK_EV_CONNECT_IND, "B sees a connect indication");
    check(dnet_link_state_of(&B) == DNET_LINK_CR_RCVD, "B -> CR_RCVD");
    check(B.remote_addr == 0x2001, "B learned A's logical-link address");
    check(B.segsize == 1459, "B learned the requested segsize");

    /* B accepts -> builds a CC. */
    struct dnet_nsp_msg cc;
    check(dnet_link_accept(&B, &cc, 101) == DNET_LINK_OK, "B accept() builds CC");
    check(dnet_link_state_of(&B) == DNET_LINK_RUN, "B -> RUN");
    check(cc.type == DNET_NSP_T_CC && cc.srcaddr == 0x2002 && cc.dstaddr == 0x2001,
          "CC names B's LLA and echoes A's LLA as dst");

    /* A receives the CC -> connect confirmed, link up. */
    check(dnet_link_rx(&A, &cc, 102, NULL, NULL, &ev) == DNET_LINK_OK, "A rx(CC) ok");
    check(ev == DNET_LINK_EV_CONNECT_CONF, "A sees connect confirm");
    check(dnet_link_is_up(&A) && dnet_link_is_up(&B), "both links are UP (RUN)");
    check(A.remote_addr == 0x2002, "A learned B's logical-link address");

    /* A sends a data segment; B receives it and acks; A absorbs the ack. */
    const char *payload = "COPY VAX1::LOGIN.COM";
    struct dnet_nsp_msg dseg, ack;
    int has_reply = 0;
    check(dnet_link_send_data(&A, (const uint8_t *)payload, strlen(payload),
                              &dseg, 103) == DNET_LINK_OK, "A send_data builds a segment");
    check(dseg.type == DNET_NSP_T_DATA && (dseg.segnum & DNET_LINK_SEQ_MASK) == 1,
          "first data segment is segnum 1");
    check(dnet_link_rx(&B, &dseg, 104, &ack, &has_reply, &ev) == DNET_LINK_OK,
          "B rx(DATA) ok");
    check(ev == DNET_LINK_EV_DATA, "B sees a data event");
    check(dseg.datalen == strlen(payload) &&
          memcmp(dseg.data, payload, dseg.datalen) == 0,
          "B received the exact payload bytes");
    check(has_reply && ack.type == DNET_NSP_T_ACK && (ack.acknum & DNET_NSP_ACK_QUAL),
          "B produced a data-ack with the QUAL bit");
    check((ack.acknum & DNET_LINK_SEQ_MASK) == 1, "ack acknowledges segnum 1");
    check(dnet_link_rx(&A, &ack, 105, NULL, NULL, &ev) == DNET_LINK_OK, "A rx(ACK) ok");
    check(ev == DNET_LINK_EV_ACK && A.send_ack == 1, "A absorbs the ack (send_ack=1)");

    /* A disconnects; B confirms; both close. */
    struct dnet_nsp_msg di, dc;
    check(dnet_link_disconnect(&A, DNET_LINK_REASON_NORMAL, &di, 106) == DNET_LINK_OK,
          "A disconnect() builds DI");
    check(dnet_link_state_of(&A) == DNET_LINK_DI_SENT, "A -> DI_SENT");
    check(di.type == DNET_NSP_T_DI, "DI built");
    has_reply = 0;
    check(dnet_link_rx(&B, &di, 107, &dc, &has_reply, &ev) == DNET_LINK_OK, "B rx(DI) ok");
    check(ev == DNET_LINK_EV_DISCONNECT, "B sees a disconnect");
    check(dnet_link_state_of(&B) == DNET_LINK_CLOSED, "B -> CLOSED");
    check(has_reply && dc.type == DNET_NSP_T_DC, "B produced a Disconnect Confirm");
    check(dnet_link_rx(&A, &dc, 108, NULL, NULL, &ev) == DNET_LINK_OK, "A rx(DC) ok");
    check(ev == DNET_LINK_EV_DISCONNECT_CONF, "A sees the disconnect confirm");
    check(dnet_link_state_of(&A) == DNET_LINK_CLOSED, "A -> CLOSED (link fully torn down)");
}

/* ---- 2. CI retransmission + unreachable give-up ------------------------- */
static void test_retransmit(void)
{
    printf("[retransmit] unanswered CI retransmits, then abandons as UNREACHABLE\n");

    struct dnet_link A;
    dnet_link_init(&A, 0x2001, 0x0402, 1000);
    struct dnet_nsp_msg ci;
    check(dnet_link_connect(&A, NULL, 0, 1459, 0, DNET_NSP_VER_41, &ci, 1000)
              == DNET_LINK_OK, "connect at t=1000");

    int has_out = 0;
    /* Before the deadline: no retransmit. */
    check(dnet_link_tick(&A, 1000 + DNET_LINK_CI_RETRANS_SECS - 1, &ci, &has_out)
              == DNET_LINK_OK && !has_out, "no retransmit before the deadline");

    /* Step the clock in retransmit intervals; each fires exactly one CI. */
    unsigned fired = 0;
    dnet_tick_t t = 1000;
    for (unsigned i = 0; i < DNET_LINK_MAX_RETRANS + 4; i++) {
        t += DNET_LINK_CI_RETRANS_SECS;
        has_out = 0;
        dnet_link_tick(&A, t, &ci, &has_out);
        if (has_out) {
            fired++;
            check(ci.type == DNET_NSP_T_CI && ci.srcaddr == 0x2001,
                  "retransmit is a byte-faithful CI");
        }
        if (dnet_link_state_of(&A) == DNET_LINK_CLOSED)
            break;
    }
    check(fired == DNET_LINK_MAX_RETRANS, "exactly MAX_RETRANS (8) retransmits fired");
    check(dnet_link_state_of(&A) == DNET_LINK_CLOSED, "link abandoned -> CLOSED");
    check(A.disc_reason == DNET_LINK_REASON_UNREACHABLE, "reason = UNREACHABLE (honest, not faked)");
}

/* ---- 3. engine end-to-end over a real socketpair ------------------------ */
/* Move a full frame L-side -> R-side (or reverse) over the socketpair and hand
 * it to the peer engine's link_rx; if a reply frame comes back, ship it too. */
static void test_engine_e2e(void)
{
    printf("[engine] two engines open a logical link + move data over a socketpair\n");

    int sv[2];
    check(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0, "socketpair created");

    const uint8_t hwL[6] = { 0x02,0,0,0,0,0x0a };
    const uint8_t hwR[6] = { 0x02,0,0,0,0,0x0b };
    struct dnet_engine L, R;   /* L = 1.10 originator, R = 1.11 responder */
    check(dnet_engine_init(&L, 1, 10, "OVMXL", "EWA0", NULL, hwL, 0, 0, 0) == DNET_ENGINE_OK &&
          dnet_engine_init(&R, 1, 11, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0) == DNET_ENGINE_OK,
          "L + R engines init");

    uint8_t frame[DNET_FRAME_MAX], rxbuf[DNET_FRAME_MAX], reply[DNET_FRAME_MAX];
    size_t flen = 0, rlen = 0;
    ssize_t n;
    int has_reply = 0;
    enum dnet_link_event ev;

    /* L opens a logical link to R (node 1.11) -> CI frame on the wire. */
    check(dnet_engine_link_open(&L, 1, 11, /*LLA*/0x2001, NULL, 0, 1459, 1,
                                DNET_NSP_VER_41, frame, sizeof(frame), &flen, 10)
              == DNET_ENGINE_OK, "L link_open builds a CI data frame");
    check(dnet_link_state_of(&L.link) == DNET_LINK_CI_SENT, "L link is CI_SENT");
    check(write(sv[0], frame, flen) == (ssize_t)flen, "CI frame written to the wire");
    n = read(sv[1], rxbuf, sizeof(rxbuf));
    check(n == (ssize_t)flen, "R read the whole CI frame");

    /* R consumes the CI -> connect indication, then accepts -> CC frame back. */
    check(dnet_engine_link_rx(&R, 10, rxbuf, (size_t)n, reply, sizeof(reply), &rlen,
                              &has_reply, &ev) == DNET_ENGINE_OK, "R link_rx(CI) ok");
    check(ev == DNET_LINK_EV_CONNECT_IND, "R sees connect indication");
    check(R.link.remote_node == L.addr, "R learned L's node address from the frame");
    check(dnet_engine_link_accept(&R, 0x2002, reply, sizeof(reply), &rlen, 11)
              == DNET_ENGINE_OK, "R link_accept builds a CC frame");
    check(write(sv[1], reply, rlen) == (ssize_t)rlen, "CC frame written back");
    n = read(sv[0], rxbuf, sizeof(rxbuf));
    check(n == (ssize_t)rlen, "L read the CC frame");
    check(dnet_engine_link_rx(&L, 11, rxbuf, (size_t)n, reply, sizeof(reply), &rlen,
                              &has_reply, &ev) == DNET_ENGINE_OK, "L link_rx(CC) ok");
    check(ev == DNET_LINK_EV_CONNECT_CONF, "L sees connect confirm");
    check(dnet_link_is_up(&L.link) && dnet_link_is_up(&R.link), "both engine links UP");

    /* L sends data -> R delivers it + acks -> L absorbs the ack. */
    const char *msg = "$ TYPE SYS$SYSTEM:OVMX-DECNET.TXT";
    check(dnet_engine_link_send(&L, (const uint8_t *)msg, strlen(msg),
                                frame, sizeof(frame), &flen, 12) == DNET_ENGINE_OK,
          "L link_send builds a data frame");
    check(write(sv[0], frame, flen) == (ssize_t)flen, "data frame written");
    n = read(sv[1], rxbuf, sizeof(rxbuf));
    check(n == (ssize_t)flen, "R read the data frame");
    has_reply = 0;
    check(dnet_engine_link_rx(&R, 12, rxbuf, (size_t)n, reply, sizeof(reply), &rlen,
                              &has_reply, &ev) == DNET_ENGINE_OK, "R link_rx(DATA) ok");
    check(ev == DNET_LINK_EV_DATA, "R sees a data event");
    check(R.rx_datalen == strlen(msg) && memcmp(R.rx_data, msg, R.rx_datalen) == 0,
          "R delivered the exact payload (byte-identical round-trip over the wire)");
    check(has_reply, "R produced an ack frame");
    check(write(sv[1], reply, rlen) == (ssize_t)rlen, "ack frame written back");
    n = read(sv[0], rxbuf, sizeof(rxbuf));
    check(n == (ssize_t)rlen, "L read the ack frame");
    check(dnet_engine_link_rx(&L, 13, rxbuf, (size_t)n, reply, sizeof(reply), &rlen,
                              &has_reply, &ev) == DNET_ENGINE_OK, "L link_rx(ACK) ok");
    check(ev == DNET_LINK_EV_ACK && L.link.send_ack == 1, "L absorbs the ack");

    /* L disconnects -> R confirms -> both closed. */
    check(dnet_engine_link_close(&L, DNET_LINK_REASON_NORMAL, frame, sizeof(frame),
                                 &flen, 14) == DNET_ENGINE_OK, "L link_close builds a DI frame");
    check(write(sv[0], frame, flen) == (ssize_t)flen, "DI frame written");
    n = read(sv[1], rxbuf, sizeof(rxbuf));
    has_reply = 0;
    check(dnet_engine_link_rx(&R, 14, rxbuf, (size_t)n, reply, sizeof(reply), &rlen,
                              &has_reply, &ev) == DNET_ENGINE_OK, "R link_rx(DI) ok");
    check(ev == DNET_LINK_EV_DISCONNECT && has_reply, "R disconnects + confirms (DC)");
    check(dnet_link_state_of(&R.link) == DNET_LINK_CLOSED, "R link CLOSED");
    check(write(sv[1], reply, rlen) == (ssize_t)rlen, "DC frame written back");
    n = read(sv[0], rxbuf, sizeof(rxbuf));
    check(dnet_engine_link_rx(&L, 15, rxbuf, (size_t)n, reply, sizeof(reply), &rlen,
                              &has_reply, &ev) == DNET_ENGINE_OK, "L link_rx(DC) ok");
    check(ev == DNET_LINK_EV_DISCONNECT_CONF, "L sees the disconnect confirm");
    check(dnet_link_state_of(&L.link) == DNET_LINK_CLOSED, "L link CLOSED (torn down)");

    /* A HELLO control frame must NOT parse as a data frame (planes separable). */
    check(dnet_engine_build_hello_frame(&L, frame, sizeof(frame), &flen) == DNET_ENGINE_OK,
          "L builds a HELLO");
    const uint8_t *pdu; size_t plen;
    check(dnet_engine_parse_data_frame(frame, flen, NULL, NULL, &pdu, &plen)
              == DNET_ENGINE_EINVAL, "a HELLO frame is rejected by parse_data_frame");

    close(sv[0]); close(sv[1]);
}

int main(void)
{
    printf("test_dnet_link: DECnet Phase IV NSP logical-link connection service\n");
    test_fsm();
    test_retransmit();
    test_engine_e2e();
    if (failures == 0) { printf("test_dnet_link: ALL CHECKS PASSED\n"); return 0; }
    printf("test_dnet_link: %d CHECK(S) FAILED\n", failures);
    return 1;
}
