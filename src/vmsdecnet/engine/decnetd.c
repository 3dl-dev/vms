/*
 * decnetd.c - the OVMX DECnet Phase IV routing ENGINE daemon (rd vms-449d,
 *             engine rung 1 of epic vms-30e).
 *
 * A userspace daemon that
 * owns a raw-L2 datalink and speaks a DEC wire protocol over it, while
 * presenting a VMS-faithful surface upward. It is the ONLY place the AF_PACKET
 * socket is touched (scs_datalink_{open,send,recv} -- the SAME generic raw-L2
 * abstraction scsd.c uses, deliberately written engine-agnostic for exactly
 * this consumer, see src/libdatalink/include/scs_datalink.h). Everything the wire
 * logic does lives in the pure, socketless engine core (dnet_engine.{c,h}),
 * which drives the three landed codecs.
 *
 * RULE 1 -- "do it like VMS, or HIDE it." The raw socket and the Linux/NetBSD
 * interface name are the HIDDEN mechanism. What this daemon puts on stdout is
 * the DECnet routing surface an NCP user sees -- an executor node, a circuit,
 * and a live adjacency table (SHOW ADJACENT NODES) -- plus DECNETD-I- facility
 * messages in the scsd house style. It never prints a raw socket or a bare
 * `ethN`, exactly as scsd never exposes its AF_PACKET fd behind the SCS face.
 *
 * OPERATOR RULING 2026-08-31 (rd vms-a1c): Option B -- userspace AF_PACKET, not
 * an in-kernel AF_DECnet forward-port. See docs/decnet-provenance-register.md
 * sec 6.
 *
 * SCOPE (rung 1, rd vms-449d): datalink open (fail-honest, INV-6) + endnode
 * HELLO transmit on the T3 cadence + receive/decode of peer HELLOs driving the
 * adjacency SM + the VMS presentation surface. NOT in this rung (filed as
 * children of vms-30e): the NSP logical-link connection service (vms-c23) and
 * the live-VAX oracle adjacency bracket (vms-aac0).
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8): wire form from public DNA Phase IV + the
 * vms-3be lab capture + OVMX's own scsd datalink pattern. No VSI/HPE source.
 */
#include <errno.h>
#include <fcntl.h>       /* fcntl() */
#include <net/if.h>      /* if_nametoindex() */
#include <poll.h>        /* poll() -- multiplex the datalink + the aux fd */
#include <pty.h>         /* openpty() -- the CTERM HOST spawns a real PTY */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>   /* TIOCSWINSZ, TIOCSCTTY */
#include <sys/socket.h>
#include <sys/wait.h>    /* waitpid() -- reap the CTERM login child */
#include <termios.h>     /* raw-mode local terminal (SET HOST client) */
#include <time.h>
#include <unistd.h>

#include "dnet_engine.h"
#include "dnet_cterm.h"     /* CTERM terminal-service protocol (--set-host-selftest) */
#include "ovmx_identity.h"  /* INV-1 identity SSOT: human banner = OVMX product id */
#include "scs_datalink.h"   /* the shared raw-L2 datalink (src/libdatalink) */

/* Default datalink interface, matching scsd's br0 default (the lab-2 pod
 * bridge model that carries raw Phase IV multicast; SLIRP cannot, see
 * docs/decnet-provenance-register.md sec 4.2). */
#define DECNETD_DEFAULT_IFACE  "br0"

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int signo) { (void)signo; g_stop = 1; }

/* A monotonic seconds tick -- the unit the engine's T3/listen timers use. */
static dnet_tick_t monotonic_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (dnet_tick_t)ts.tv_sec;
}

static void log_ts(FILE *out)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    time_t t = ts.tv_sec;
    gmtime_r(&t, &tmv);
    char b[32];
    strftime(b, sizeof(b), "%d-%b-%Y %H:%M:%S", &tmv);
    fprintf(out, "%s", b);
}

/* Parse "area.node" (e.g. "1.42"). Returns 0 on success. */
static int parse_addr(const char *s, unsigned *area, unsigned *node)
{
    if (!s)
        return -1;
    char *end = NULL;
    long a = strtol(s, &end, 10);
    if (end == s || *end != '.')
        return -1;
    char *end2 = NULL;
    long n = strtol(end + 1, &end2, 10);
    if (end2 == end + 1 || *end2 != '\0')
        return -1;
    if (a < 1 || a > 63 || n < 1 || n > 1023)
        return -1;
    *area = (unsigned)a;
    *node = (unsigned)n;
    return 0;
}

/*
 * ============================ --self-test =============================
 * A no-privilege, no-netdev proof that the engine really MOVES a HELLO frame
 * and drives adjacency: it stands up TWO engines (a "left" and a "right"
 * node) and shuttles their built HELLO frames between them over a real
 * socketpair(2) -- genuine write(2)/read(2) of the actual encoded bytes, not
 * an in-memory handoff -- then asserts the adjacency SM advances. This is the
 * DECnet analogue of scsd's --dlm-selftest: it runs anywhere (Docker/CI, no
 * CAP_NET_RAW), and it exercises the exact build_hello_frame -> wire ->
 * rx_frame path the live datalink uses, so a green self-test is a real
 * tx/rx/decode/SM proof, not a facade (INV-6).
 *
 * Returns 0 on PASS, 1 on FAIL.
 */
static int run_self_test(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        fprintf(stderr, "DECNETD-E-SELFTEST, socketpair failed: %s\n",
                strerror(errno));
        return 1;
    }

    const uint8_t hwL[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
    const uint8_t hwR[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };
    struct dnet_engine L, R;
    /* left = 1.10 (OVMXL), right = 1.11 (OVMXR); both endnodes. */
    if (dnet_engine_init(&L, 1, 10, "OVMXL", "EWA0", NULL, hwL, 0, 0, 0) != 0 ||
        dnet_engine_init(&R, 1, 11, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0) != 0) {
        fprintf(stderr, "DECNETD-E-SELFTEST, engine init failed\n");
        close(sv[0]); close(sv[1]);
        return 1;
    }

    int fail = 0;
    uint8_t frame[DNET_FRAME_MAX];
    uint8_t rxbuf[DNET_FRAME_MAX];
    size_t flen = 0;
    ssize_t n;
    enum dnet_adj_state st = DNET_ADJ_DOWN;
    uint8_t from[6];
    dnet_tick_t now = 100;

    /* 1) LEFT emits a plain endnode HELLO -> RIGHT. RIGHT should move the
     *    neighbour to INITIALIZING (one-way: our HELLO names no router). */
    if (dnet_engine_build_hello_frame(&L, frame, sizeof(frame), &flen) != 0) {
        fprintf(stderr, "DECNETD-E-SELFTEST, build HELLO (L) failed\n");
        fail = 1; goto done;
    }
    if (write(sv[0], frame, flen) != (ssize_t)flen) {
        fprintf(stderr, "DECNETD-E-SELFTEST, write failed: %s\n", strerror(errno));
        fail = 1; goto done;
    }
    n = read(sv[1], rxbuf, sizeof(rxbuf));
    if (n <= 0) {
        fprintf(stderr, "DECNETD-E-SELFTEST, read failed: %s\n", strerror(errno));
        fail = 1; goto done;
    }
    if (dnet_engine_rx_frame(&R, now, rxbuf, (size_t)n, from, &st) != 1 ||
        st != DNET_ADJ_INITIALIZING) {
        fprintf(stderr, "DECNETD-E-SELFTEST, R did not reach INITIALIZING (st=%d)\n",
                (int)st);
        fail = 1; goto done;
    }

    /* 2) RIGHT emits a HELLO that NAMES LEFT as its neighbour (the two-way
     *    handshake). Feed it to a fresh view on LEFT: LEFT should reach UP. */
    {
        /* Build RIGHT's HELLO, then overwrite its routing neighbour field so it
         * names LEFT -- the DNA two-way reachability signal. We do this at the
         * decoded-struct layer via the codec to stay honest to the wire form. */
        struct dnet_endnode_hello h;
        memset(&h, 0, sizeof(h));
        h.rflags  = DNET_RFLAG_ENDNODE_HELLO;
        h.version = 2;
        memcpy(h.id, R.my_id, 6);
        h.iinfo   = DNET_NODETYPE_ENDNODE;
        h.blksize = 1498;
        h.timer   = 15;
        memcpy(h.neighbor, L.my_id, 6);   /* names LEFT => two-way */
        h.datalen = 0;
        uint8_t payload[DNET_FRAME_MAX];
        size_t plen = 0;
        if (dnet_hello_encode(&h, payload, sizeof(payload), &plen) != DNET_HELLO_OK) {
            fprintf(stderr, "DECNETD-E-SELFTEST, encode (R two-way) failed\n");
            fail = 1; goto done;
        }
        /* Prepend the Ethernet header (dst=mcast, src=R id, type 0x6003). */
        uint8_t f2[DNET_FRAME_MAX];
        memcpy(f2, DNET_HELLO_MCAST, 6);
        memcpy(f2 + 6, R.my_id, 6);
        f2[12] = 0x60; f2[13] = 0x03;
        memcpy(f2 + DNET_ETH_HDRLEN, payload, plen);
        size_t f2len = DNET_ETH_HDRLEN + plen;

        if (write(sv[1], f2, f2len) != (ssize_t)f2len) {
            fprintf(stderr, "DECNETD-E-SELFTEST, write2 failed\n");
            fail = 1; goto done;
        }
        n = read(sv[0], rxbuf, sizeof(rxbuf));
        if (n <= 0) {
            fprintf(stderr, "DECNETD-E-SELFTEST, read2 failed\n");
            fail = 1; goto done;
        }
        st = DNET_ADJ_DOWN;
        if (dnet_engine_rx_frame(&L, now + 1, rxbuf, (size_t)n, from, &st) != 1 ||
            st != DNET_ADJ_UP) {
            fprintf(stderr, "DECNETD-E-SELFTEST, L did not reach UP (st=%d)\n",
                    (int)st);
            fail = 1; goto done;
        }
    }

    /* 3) LEFT's neighbour (RIGHT) must age to DOWN once the listen timer lapses
     *    with no further HELLO (T4 = BCT3MULT * T3 = 30 s). */
    if (dnet_engine_tick(&L, now + 1 + 31) < 1 ||
        dnet_adj_state_of(&L.adj, R.my_id) != DNET_ADJ_DOWN) {
        fprintf(stderr, "DECNETD-E-SELFTEST, L neighbour did not age to DOWN\n");
        fail = 1; goto done;
    }

    /* 4) An echo of our OWN frame must be ignored (src == my_id). */
    if (dnet_engine_build_hello_frame(&L, frame, sizeof(frame), &flen) != 0 ||
        dnet_engine_rx_frame(&L, now + 40, frame, flen, NULL, NULL) != 0) {
        fprintf(stderr, "DECNETD-E-SELFTEST, own-echo was not ignored\n");
        fail = 1; goto done;
    }

done:
    close(sv[0]);
    close(sv[1]);
    if (fail) {
        printf("DECNETD-SELFTEST: FAIL\n");
        return 1;
    }
    printf("DECNETD-I-SELFTEST, engine tx/rx/adjacency proof PASSED"
           " (HELLO moved over a real socketpair; INITIALIZING->UP->DOWN + own-echo drop)\n");
    return 0;
}

/*
 * ========================== --nsp-selftest ===========================
 * A no-privilege, no-netdev proof of the NSP LOGICAL-LINK connection service
 * (rd vms-c23, engine rung 2): two engines OPEN a logical link, exchange a data
 * segment with its acknowledgement, and DISCONNECT cleanly -- all as full
 * on-wire data frames (Ethernet + Phase IV long-data routing header + NSP PDU)
 * shuttled over a real socketpair(2). It exercises the exact
 * link_open/link_send/link_close -> build_data_frame -> wire -> link_rx path a
 * live datalink uses, so a green run is a real connection-service proof, not a
 * facade (INV-6). Returns 0 on PASS, 1 on FAIL.
 */
static int move_frame(int wfd, int rfd, const uint8_t *frame, size_t flen,
                      uint8_t *rxbuf, size_t rxcap, size_t *rxlen)
{
    if (write(wfd, frame, flen) != (ssize_t)flen)
        return -1;
    ssize_t n = read(rfd, rxbuf, rxcap);
    if (n <= 0)
        return -1;
    *rxlen = (size_t)n;
    return 0;
}

static int run_nsp_selftest(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        fprintf(stderr, "DECNETD-E-SELFTEST, socketpair failed: %s\n", strerror(errno));
        return 1;
    }
    const uint8_t hwL[6] = { 0x02,0,0,0,0,0x01 };
    const uint8_t hwR[6] = { 0x02,0,0,0,0,0x02 };
    struct dnet_engine L, R;   /* L = 1.10 originator, R = 1.11 responder */
    if (dnet_engine_init(&L, 1, 10, "OVMXL", "EWA0", NULL, hwL, 0, 0, 0) != 0 ||
        dnet_engine_init(&R, 1, 11, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0) != 0) {
        fprintf(stderr, "DECNETD-E-SELFTEST, engine init failed\n");
        close(sv[0]); close(sv[1]);
        return 1;
    }

    uint8_t frame[DNET_FRAME_MAX], rxbuf[DNET_FRAME_MAX], reply[DNET_FRAME_MAX];
    size_t flen = 0, rlen = 0, rxlen = 0;
    int has_reply = 0, fail = 0;
    enum dnet_link_event ev = DNET_LINK_EV_NONE;
    const char *payload = "$ DIRECTORY OVMXR::SYS$LOGIN:";

    /* 1) L opens a logical link to R (node 1.11) -> CI frame -> R connect ind. */
    if (dnet_engine_link_open(&L, 1, 11, 0x2001, NULL, 0, 1459, 1,
                              DNET_NSP_VER_41, frame, sizeof(frame), &flen, 10) != 0 ||
        move_frame(sv[0], sv[1], frame, flen, rxbuf, sizeof(rxbuf), &rxlen) != 0 ||
        dnet_engine_link_rx(&R, 10, rxbuf, rxlen, reply, sizeof(reply), &rlen,
                            &has_reply, &ev) != 0 || ev != DNET_LINK_EV_CONNECT_IND) {
        fprintf(stderr, "DECNETD-E-SELFTEST, connect-initiate did not reach R\n");
        fail = 1; goto done;
    }
    /* 2) R accepts -> CC frame -> L sees the link RUN. */
    if (dnet_engine_link_accept(&R, 0x2002, reply, sizeof(reply), &rlen, 11) != 0 ||
        move_frame(sv[1], sv[0], reply, rlen, rxbuf, sizeof(rxbuf), &rxlen) != 0 ||
        dnet_engine_link_rx(&L, 11, rxbuf, rxlen, frame, sizeof(frame), &flen,
                            &has_reply, &ev) != 0 || ev != DNET_LINK_EV_CONNECT_CONF ||
        !dnet_link_is_up(&L.link) || !dnet_link_is_up(&R.link)) {
        fprintf(stderr, "DECNETD-E-SELFTEST, connect-confirm did not bring the link UP\n");
        fail = 1; goto done;
    }
    /* 3) L sends data -> R delivers it byte-identical + acks -> L absorbs it. */
    if (dnet_engine_link_send(&L, (const uint8_t *)payload, strlen(payload),
                              frame, sizeof(frame), &flen, 12) != 0 ||
        move_frame(sv[0], sv[1], frame, flen, rxbuf, sizeof(rxbuf), &rxlen) != 0 ||
        dnet_engine_link_rx(&R, 12, rxbuf, rxlen, reply, sizeof(reply), &rlen,
                            &has_reply, &ev) != 0 || ev != DNET_LINK_EV_DATA ||
        R.rx_datalen != strlen(payload) ||
        memcmp(R.rx_data, payload, R.rx_datalen) != 0 || !has_reply) {
        fprintf(stderr, "DECNETD-E-SELFTEST, data segment did not round-trip\n");
        fail = 1; goto done;
    }
    if (move_frame(sv[1], sv[0], reply, rlen, rxbuf, sizeof(rxbuf), &rxlen) != 0 ||
        dnet_engine_link_rx(&L, 13, rxbuf, rxlen, frame, sizeof(frame), &flen,
                            &has_reply, &ev) != 0 || ev != DNET_LINK_EV_ACK) {
        fprintf(stderr, "DECNETD-E-SELFTEST, data ack did not return\n");
        fail = 1; goto done;
    }
    /* 4) L disconnects -> R confirms (DC) -> both CLOSED. */
    if (dnet_engine_link_close(&L, DNET_LINK_REASON_NORMAL, frame, sizeof(frame),
                               &flen, 14) != 0 ||
        move_frame(sv[0], sv[1], frame, flen, rxbuf, sizeof(rxbuf), &rxlen) != 0 ||
        dnet_engine_link_rx(&R, 14, rxbuf, rxlen, reply, sizeof(reply), &rlen,
                            &has_reply, &ev) != 0 || ev != DNET_LINK_EV_DISCONNECT ||
        dnet_link_state_of(&R.link) != DNET_LINK_CLOSED || !has_reply ||
        move_frame(sv[1], sv[0], reply, rlen, rxbuf, sizeof(rxbuf), &rxlen) != 0 ||
        dnet_engine_link_rx(&L, 15, rxbuf, rxlen, frame, sizeof(frame), &flen,
                            &has_reply, &ev) != 0 || ev != DNET_LINK_EV_DISCONNECT_CONF ||
        dnet_link_state_of(&L.link) != DNET_LINK_CLOSED) {
        fprintf(stderr, "DECNETD-E-SELFTEST, disconnect handshake did not close cleanly\n");
        fail = 1; goto done;
    }

done:
    close(sv[0]); close(sv[1]);
    if (fail) {
        printf("DECNETD-NSP-SELFTEST: FAIL\n");
        return 1;
    }
    printf("DECNETD-I-NSPSELFTEST, NSP logical-link connection proof PASSED"
           " (link OPEN -> data segment+ack -> clean DISCONNECT over a real"
           " socketpair; CI/CC + DI/DC choreography, payload byte-identical)\n");
    return 0;
}

/*
 * ======================== --set-host-selftest ========================
 * A no-privilege, no-netdev proof of the CTERM (Command Terminal) protocol
 * behind $ SET HOST (rd vms-4d2, engine rung 3): two engines open an NSP logical
 * link to the CTERM object (42) and carry a WHOLE terminal session over it --
 * Bind -> Bind Accept -> terminal characteristics -> a host screen Write -> a
 * terminal keystroke Read Data -> an out-of-band ^Y -> Unbind -> link teardown --
 * as real on-wire NSP data frames shuttled over a socketpair(2), every CTERM
 * payload round-tripping byte-identical. It exercises the exact
 * cterm_* -> link_send -> build_data_frame -> wire -> link_rx -> cterm_rx path a
 * live $ SET HOST uses, so a green run is a real terminal-service proof, not a
 * facade (INV-6). CLEAN-ROOM (Rule 8): CTERM is spec-derived (no oracle
 * specimen). Returns 0 on PASS, 1 on FAIL.
 */
/* Send a CTERM PDU as an NSP data segment L->R (dir=0) or R->L (dir=1), deliver
 * it to the peer CTERM session, and absorb the NSP ack the segment generates.
 * Returns the CTERM event, or a negative value on a wire/protocol failure. */
static int sethost_ship(int sv[2], int dir, struct dnet_engine *tx,
                        struct dnet_engine *rx, struct dnet_cterm_session *rx_sess,
                        const uint8_t *pdu, size_t plen, dnet_tick_t now)
{
    uint8_t frame[DNET_FRAME_MAX], rxbuf[DNET_FRAME_MAX], reply[DNET_FRAME_MAX];
    size_t flen = 0, rlen = 0, rxlen = 0;
    int has_reply = 0, wfd = sv[dir], rfd = sv[dir ^ 1];
    enum dnet_link_event lev = DNET_LINK_EV_NONE;
    enum dnet_cterm_event cev = DNET_CTERM_EV_NONE;

    if (dnet_engine_link_send(tx, pdu, plen, frame, sizeof(frame), &flen, now) != 0 ||
        move_frame(wfd, rfd, frame, flen, rxbuf, sizeof(rxbuf), &rxlen) != 0 ||
        dnet_engine_link_rx(rx, now, rxbuf, rxlen, reply, sizeof(reply), &rlen,
                            &has_reply, &lev) != 0 || lev != DNET_LINK_EV_DATA)
        return -1;
    if (dnet_cterm_rx(rx_sess, rx->rx_data, rx->rx_datalen, &cev) != DNET_CTERM_OK)
        return -1;
    /* Absorb the NSP data-ack back to the sender so sequencing stays honest. */
    if (has_reply) {
        if (move_frame(rfd, wfd, reply, rlen, rxbuf, sizeof(rxbuf), &rxlen) != 0 ||
            dnet_engine_link_rx(tx, now, rxbuf, rxlen, frame, sizeof(frame), &flen,
                                &has_reply, &lev) != 0)
            return -1;
    }
    return (int)cev;
}

static int run_sethost_selftest(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        fprintf(stderr, "DECNETD-E-SELFTEST, socketpair failed: %s\n", strerror(errno));
        return 1;
    }
    const uint8_t hwL[6] = { 0x02,0,0,0,0,0x01 };
    const uint8_t hwR[6] = { 0x02,0,0,0,0,0x02 };
    struct dnet_engine L, R;   /* L = 2.10 SET HOST initiator, R = 2.11 host */
    struct dnet_cterm_session term, host;
    if (dnet_engine_init(&L, 2, 10, "OVMXL", "EWA0", NULL, hwL, 0, 0, 0) != 0 ||
        dnet_engine_init(&R, 2, 11, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0) != 0 ||
        dnet_cterm_session_init(&term, DNET_CTERM_ROLE_TERMINAL) != 0 ||
        dnet_cterm_session_init(&host, DNET_CTERM_ROLE_HOST) != 0) {
        fprintf(stderr, "DECNETD-E-SELFTEST, init failed\n");
        close(sv[0]); close(sv[1]);
        return 1;
    }

    uint8_t frame[DNET_FRAME_MAX], rxbuf[DNET_FRAME_MAX], reply[DNET_FRAME_MAX];
    uint8_t cpdu[DNET_CTERM_MAX_PDU], sc[128];
    size_t flen = 0, rlen = 0, rxlen = 0, clen = 0, sclen = 0;
    int has_reply = 0, fail = 0;
    enum dnet_link_event lev = DNET_LINK_EV_NONE;
    dnet_tick_t t = 100;

    /* 1) open the logical link to the CTERM object (CI carries SC connect #42). */
    if (dnet_cterm_sc_connect_build(DNET_CTERM_OBJECT, "SYSTEM", "", "",
                                    sc, sizeof(sc), &sclen) != 0 ||
        dnet_engine_link_open(&L, 2, 11, 0x2001, sc, sclen, 1459, 1,
                              DNET_NSP_VER_41, frame, sizeof(frame), &flen, t) != 0 ||
        move_frame(sv[0], sv[1], frame, flen, rxbuf, sizeof(rxbuf), &rxlen) != 0 ||
        dnet_engine_link_rx(&R, t, rxbuf, rxlen, reply, sizeof(reply), &rlen,
                            &has_reply, &lev) != 0 || lev != DNET_LINK_EV_CONNECT_IND ||
        dnet_cterm_sc_connect_object(R.link.conn_data, R.link.conn_len) != DNET_CTERM_OBJECT) {
        fprintf(stderr, "DECNETD-E-SETHOST, connect to the CTERM object failed\n");
        fail = 1; goto done;
    }
    /* R accepts -> CC -> L link RUN. */
    if (dnet_engine_link_accept(&R, 0x2002, reply, sizeof(reply), &rlen, t) != 0 ||
        move_frame(sv[1], sv[0], reply, rlen, rxbuf, sizeof(rxbuf), &rxlen) != 0 ||
        dnet_engine_link_rx(&L, t, rxbuf, rxlen, frame, sizeof(frame), &flen,
                            &has_reply, &lev) != 0 || lev != DNET_LINK_EV_CONNECT_CONF ||
        !dnet_link_is_up(&L.link) || !dnet_link_is_up(&R.link)) {
        fprintf(stderr, "DECNETD-E-SETHOST, logical link did not come UP\n");
        fail = 1; goto done;
    }
    t++;

    /* 2) CTERM Bind / Bind Accept -> both sessions BOUND. */
    if (dnet_cterm_bind(&term, "OVMXL$RTA1:", cpdu, sizeof(cpdu), &clen) != 0 ||
        sethost_ship(sv, 0, &L, &R, &host, cpdu, clen, t) != DNET_CTERM_EV_BIND_IND) {
        fprintf(stderr, "DECNETD-E-SETHOST, CTERM Bind did not reach the host\n");
        fail = 1; goto done;
    }
    t++;
    if (dnet_cterm_bind_accept(&host, "OVMXR", cpdu, sizeof(cpdu), &clen) != 0 ||
        sethost_ship(sv, 1, &R, &L, &term, cpdu, clen, t) != DNET_CTERM_EV_BOUND ||
        !dnet_cterm_is_bound(&term) || !dnet_cterm_is_bound(&host)) {
        fprintf(stderr, "DECNETD-E-SETHOST, CTERM session did not bind\n");
        fail = 1; goto done;
    }
    t++;

    /* 3) terminal characteristics; host screen output; terminal keystrokes; OOB. */
    if (dnet_cterm_send_characteristics(&term, 4, 132, 24,
            DNET_CTERM_CH_ECHO | DNET_CTERM_CH_WRAP, cpdu, sizeof(cpdu), &clen) != 0 ||
        sethost_ship(sv, 0, &L, &R, &host, cpdu, clen, t) != DNET_CTERM_EV_CHARACTERISTICS ||
        host.width != 132 || host.page != 24) {
        fprintf(stderr, "DECNETD-E-SETHOST, characteristics negotiation failed\n");
        fail = 1; goto done;
    }
    t++;
    /* The host's login banner is a HUMAN surface: INV-1 says it derives from the
     * identity SSOT, and INV-0 says an OVMX node announces its OWN product
     * identity ("OpenVMX ... - OpenVMS-compatible"), never bare "OpenVMS", which
     * would be passing-off. (When the peer is a REAL VMS host the banner is
     * whatever that host sends -- CTERM carries it verbatim; here the host is an
     * OVMX node, so it announces OVMX.) */
    const char *banner = "    " OVMX_PRODUCT_BANNER "\r\nUsername: ";
    if (dnet_cterm_write(&host, (const uint8_t *)banner, strlen(banner),
            DNET_CTERM_WR_NOFORMAT, cpdu, sizeof(cpdu), &clen) != 0 ||
        sethost_ship(sv, 1, &R, &L, &term, cpdu, clen, t) != DNET_CTERM_EV_WRITE ||
        term.last.datalen != strlen(banner) ||
        memcmp(term.last.data, banner, term.last.datalen) != 0) {
        fprintf(stderr, "DECNETD-E-SETHOST, host screen output did not round-trip\n");
        fail = 1; goto done;
    }
    t++;
    const char *keys = "SYSTEM";
    if (dnet_cterm_read_data(&term, (const uint8_t *)keys, strlen(keys), 0x0d,
            cpdu, sizeof(cpdu), &clen) != 0 ||
        sethost_ship(sv, 0, &L, &R, &host, cpdu, clen, t) != DNET_CTERM_EV_READ_DATA ||
        host.last.datalen != strlen(keys) ||
        memcmp(host.last.data, keys, host.last.datalen) != 0) {
        fprintf(stderr, "DECNETD-E-SETHOST, terminal keystrokes did not round-trip\n");
        fail = 1; goto done;
    }
    t++;
    if (dnet_cterm_oob(&term, 0x19, cpdu, sizeof(cpdu), &clen) != 0 ||
        sethost_ship(sv, 0, &L, &R, &host, cpdu, clen, t) != DNET_CTERM_EV_OOB ||
        host.last.oob_char != 0x19) {
        fprintf(stderr, "DECNETD-E-SETHOST, out-of-band did not round-trip\n");
        fail = 1; goto done;
    }
    t++;

    /* 4) host unbinds; then tear the NSP logical link down (DI/DC). */
    if (dnet_cterm_unbind(&host, DNET_CTERM_UNBIND_NORMAL, cpdu, sizeof(cpdu), &clen) != 0 ||
        sethost_ship(sv, 1, &R, &L, &term, cpdu, clen, t) != DNET_CTERM_EV_UNBOUND ||
        dnet_cterm_state_of(&term) != DNET_CTERM_S_UNBOUND) {
        fprintf(stderr, "DECNETD-E-SETHOST, Unbind did not release the session\n");
        fail = 1; goto done;
    }
    t++;
    if (dnet_engine_link_close(&L, DNET_LINK_REASON_NORMAL, frame, sizeof(frame), &flen, t) != 0 ||
        move_frame(sv[0], sv[1], frame, flen, rxbuf, sizeof(rxbuf), &rxlen) != 0 ||
        dnet_engine_link_rx(&R, t, rxbuf, rxlen, reply, sizeof(reply), &rlen,
                            &has_reply, &lev) != 0 || lev != DNET_LINK_EV_DISCONNECT ||
        move_frame(sv[1], sv[0], reply, rlen, rxbuf, sizeof(rxbuf), &rxlen) != 0 ||
        dnet_engine_link_rx(&L, t, rxbuf, rxlen, frame, sizeof(frame), &flen,
                            &has_reply, &lev) != 0 || lev != DNET_LINK_EV_DISCONNECT_CONF ||
        dnet_link_state_of(&L.link) != DNET_LINK_CLOSED) {
        fprintf(stderr, "DECNETD-E-SETHOST, logical link did not tear down cleanly\n");
        fail = 1; goto done;
    }

done:
    close(sv[0]); close(sv[1]);
    if (fail) {
        printf("DECNETD-SETHOST-SELFTEST: FAIL\n");
        return 1;
    }
    printf("DECNETD-I-SETHOSTSELFTEST, $ SET HOST / CTERM terminal-service proof"
           " PASSED (link to CTERM object 42 -> Bind/Accept -> characteristics ->"
           " screen output + keystrokes + out-of-band -> Unbind -> clean"
           " disconnect over a real socketpair; every CTERM payload"
           " byte-identical)\n");
    return 0;
}

/*
 * ================= LIVE $ SET HOST / CTERM over the wire ==================
 * The three selftests above prove the CTERM choreography over a socketpair with
 * SCRIPTED payloads. The two modes below carry a REAL interactive session over
 * the LIVE NSP logical link: a genuine PTY + a real login-command on the HOST
 * side, real stdin/stdout on the TERMINAL (client) side. NOTHING is canned --
 * every screen byte is the spawned program's real output and every keystroke is
 * real local input; every CTERM/NSP wire field is read from session/FSM state,
 * never copied frame-to-frame (INV-6, executive-backed-not-wire-plumbing). The
 * loops poll() BOTH the datalink AND the aux fd (stdin / pty master) on a ~1 s
 * timeout so the HELLO cadence + link tick keep firing.
 */

/* Emit the periodic engine traffic (HELLO on the T3 cadence, adjacency aging,
 * and the active link's Connect-Initiate retransmit) -- the same cadence the
 * routing loop runs, so a live SET HOST node stays a well-behaved Phase IV
 * endnode while the terminal session is up. */
static void dnet_periodic(struct dnet_engine *eng, int sock, unsigned ifindex,
                          dnet_tick_t now)
{
    if (dnet_engine_hello_due(eng, now)) {
        uint8_t frame[DNET_FRAME_MAX];
        size_t flen = 0;
        if (dnet_engine_build_hello_frame(eng, frame, sizeof(frame), &flen)
                == DNET_ENGINE_OK &&
            scs_datalink_send(sock, (int)ifindex, DNET_ETHERTYPE,
                              DNET_HELLO_MCAST, frame, flen) >= 0)
            dnet_engine_hello_emitted(eng, now);
    }
    dnet_engine_tick(eng, now);
    if (eng->link_active) {
        uint8_t frame[DNET_FRAME_MAX];
        size_t tlen = 0;
        int thas = 0;
        if (dnet_engine_link_tick(eng, now, frame, sizeof(frame), &tlen, &thas)
                == DNET_ENGINE_OK && thas) {
            uint8_t dst[DNET_ADDR_LEN];
            memcpy(dst, frame, DNET_ADDR_LEN);
            scs_datalink_send(sock, (int)ifindex, DNET_ETHERTYPE, dst, frame, tlen);
        }
    }
}

/* Ship one CTERM PDU as an NSP data segment on the live link: the FSM builds the
 * data frame (its own sequence/addressing, executive-backed), then it goes out
 * to the peer's DECnet id (frame[0..5], the routing dst the FSM wrote). Returns
 * 0 or -1. */
static int cterm_link_send(struct dnet_engine *eng, int sock, unsigned ifindex,
                           const uint8_t *pdu, size_t plen, dnet_tick_t now)
{
    uint8_t frame[DNET_FRAME_MAX];
    size_t flen = 0;
    if (dnet_engine_link_send(eng, pdu, plen, frame, sizeof(frame), &flen, now)
            != DNET_ENGINE_OK)
        return -1;
    uint8_t dst[DNET_ADDR_LEN];
    memcpy(dst, frame, DNET_ADDR_LEN);
    return scs_datalink_send(sock, (int)ifindex, DNET_ETHERTYPE, dst, frame, flen)
               < 0 ? -1 : 0;
}

/* Receive one frame and route it: an NSP long-data frame addressed to us drives
 * the logical-link FSM (auto-replies -- data ack / Disconnect Confirm -- are sent
 * here), and its higher-layer event is returned (>=0). A HELLO / adjacency frame
 * is consumed so the peer's HELLOs are honoured, returning DNET_LINK_EV_NONE. An
 * own-echo or a frame addressed elsewhere returns NONE. Returns -1 on a hard recv
 * error (caller should stop). On DNET_LINK_EV_DATA the payload is in
 * eng->rx_data / eng->rx_datalen. */
static int dnet_recv_route(struct dnet_engine *eng, int sock, unsigned ifindex,
                           dnet_tick_t now, uint8_t *rxbuf, size_t rxcap)
{
    ssize_t n = scs_datalink_recv(sock, rxbuf, rxcap);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return DNET_LINK_EV_NONE;
        return -1;
    }
    if ((size_t)n >= DNET_ETH_HDRLEN &&
        memcmp(rxbuf + 6, eng->my_id, DNET_ADDR_LEN) == 0)
        return DNET_LINK_EV_NONE;   /* our own transmitted frame */

    int is_nsp = ((size_t)n > (size_t)DNET_ETH_HDRLEN + DNET_DATA_LENPREFIX) &&
                 rxbuf[DNET_ETH_HDRLEN + DNET_DATA_LENPREFIX] == DNET_RFLAG_LONG_DATA;
    if (is_nsp) {
        if (memcmp(rxbuf, eng->my_id, DNET_ADDR_LEN) != 0)
            return DNET_LINK_EV_NONE;   /* unicast for another node */
        uint8_t frame[DNET_FRAME_MAX];
        size_t rlen = 0;
        int has_reply = 0;
        enum dnet_link_event ev = DNET_LINK_EV_NONE;
        if (dnet_engine_link_rx(eng, now, rxbuf, (size_t)n, frame, sizeof(frame),
                                &rlen, &has_reply, &ev) != DNET_ENGINE_OK)
            return DNET_LINK_EV_NONE;
        if (has_reply) {
            uint8_t dst[DNET_ADDR_LEN];
            memcpy(dst, frame, DNET_ADDR_LEN);
            scs_datalink_send(sock, (int)ifindex, DNET_ETHERTYPE, dst, frame, rlen);
        }
        return (int)ev;
    }

    uint8_t from[DNET_ADDR_LEN];
    enum dnet_adj_state st = DNET_ADJ_DOWN;
    dnet_engine_rx_frame(eng, now, rxbuf, (size_t)n, from, &st);
    return DNET_LINK_EV_NONE;
}

/* --- SET HOST client: local terminal in raw mode, save/restore on exit ----- */
static struct termios g_saved_tio;
static int g_tio_saved = 0;
static void restore_local_tty(void)
{
    if (g_tio_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_tio);
        g_tio_saved = 0;
    }
}

/*
 * --set-host AREA.NODE : the CTERM TERMINAL (the $ SET HOST client). Opens the
 * logical link to the peer's CTERM object (42), binds a terminal session,
 * negotiates characteristics, then bridges the LOCAL terminal to the remote
 * session -- real stdin keystrokes -> CTERM Read Data, remote CTERM Write ->
 * real stdout -- until the host unbinds, the link drops, or the run ends. It
 * poll()s stdin AND the datalink so the HELLO cadence + link tick keep firing.
 */
static int run_set_host_loop(struct dnet_engine *eng, int sock, unsigned ifindex,
                             const char *peer_s, const char *user)
{
    unsigned parea = 0, pnode = 0;
    if (parse_addr(peer_s, &parea, &pnode) != 0) {
        fprintf(stderr, "DECNETD-E-BADPEER, --set-host wants AREA.NODE"
                        " (1..63 . 1..1023)\n");
        return 1;
    }
    struct dnet_cterm_session term;
    if (dnet_cterm_session_init(&term, DNET_CTERM_ROLE_TERMINAL) != 0) {
        fprintf(stderr, "DECNETD-E-CTERMINIT, terminal session init failed\n");
        return 1;
    }
    /* The access-control username is the explicit --user (VMS SET HOST/USERNAME=
     * analog), defaulting to SYSTEM (the vms-3be capture observed SYSTEM in the
     * connect message). It is never read from the process environment -- see the
     * vms-cb5 identity-environment census. */
    if (!user || !*user)
        user = "SYSTEM";
    uint8_t sc[128];
    size_t sclen = 0;
    if (dnet_cterm_sc_connect_build(DNET_CTERM_OBJECT, user, "", "",
                                    sc, sizeof(sc), &sclen) != 0) {
        fprintf(stderr, "DECNETD-E-SCBUILD, CTERM connect-data build failed\n");
        return 1;
    }

    uint8_t frame[DNET_FRAME_MAX], rxbuf[DNET_FRAME_MAX], cpdu[DNET_CTERM_MAX_PDU];
    size_t flen = 0, clen = 0;
    dnet_tick_t now = monotonic_sec();
    if (dnet_engine_link_open(eng, parea, pnode, 0x2001, sc, sclen, 1459, 1,
                              DNET_NSP_VER_41, frame, sizeof(frame), &flen, now)
            != DNET_ENGINE_OK) {
        fprintf(stderr, "DECNETD-E-NOCONNECT, could not open a logical link"
                        " to %u.%u\n", parea, pnode);
        return 1;
    }
    {
        uint8_t dst[DNET_ADDR_LEN];
        dnet_id_from_addr(parea, pnode, dst);
        scs_datalink_send(sock, (int)ifindex, DNET_ETHERTYPE, dst, frame, flen);
    }
    log_ts(stdout);
    printf(" DECNETD-I-SETHOST, $ SET HOST %u.%u -- Connect Initiate sent to"
           " CTERM object %d on circuit %s\n",
           parea, pnode, DNET_CTERM_OBJECT, eng->circuit);
    fflush(stdout);

    int raw_on = 0, stdin_eof = 0, done = 0, rc = 0;

    while (!g_stop && !done) {
        now = monotonic_sec();
        dnet_periodic(eng, sock, ifindex, now);

        /* Connect-Initiate give-up: the FSM closed the link before we ever
         * bound -- the peer never answered. Report honestly and stop. */
        if (eng->link_active &&
            dnet_link_state_of(&eng->link) == DNET_LINK_CLOSED &&
            dnet_cterm_state_of(&term) == DNET_CTERM_S_CLOSED) {
            log_ts(stdout);
            printf(" DECNETD-W-UNREACH, peer %u.%u did not answer -- SET HOST"
                   " abandoned\n", parea, pnode);
            fflush(stdout);
            eng->link_active = 0;
            rc = 1;
            break;
        }

        struct pollfd pfd[2];
        int nfd = 0;
        pfd[nfd].fd = sock;          pfd[nfd].events = POLLIN; pfd[nfd].revents = 0; nfd++;
        if (dnet_cterm_is_bound(&term) && !stdin_eof) {
            pfd[nfd].fd = STDIN_FILENO; pfd[nfd].events = POLLIN; pfd[nfd].revents = 0; nfd++;
        }
        int pr = poll(pfd, (nfds_t)nfd, 1000);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "DECNETD-E-POLL, poll failed: %s\n", strerror(errno));
            rc = 1;
            break;
        }
        if (pr == 0)
            continue;

        if (pfd[0].revents & POLLIN) {
            int ev = dnet_recv_route(eng, sock, ifindex, now, rxbuf, sizeof(rxbuf));
            if (ev < 0) {
                fprintf(stderr, "DECNETD-E-RECVFAIL, recv failed: %s\n",
                        strerror(errno));
                rc = 1;
                break;
            }
            switch (ev) {
            case DNET_LINK_EV_CONNECT_CONF:
                log_ts(stdout);
                printf(" DECNETD-I-LINKUP, logical link to %u.%u is RUN --"
                       " sending CTERM Bind\n", parea, pnode);
                fflush(stdout);
                if (dnet_cterm_bind(&term, "OVMX$RTA1:", cpdu, sizeof(cpdu), &clen) != 0 ||
                    cterm_link_send(eng, sock, ifindex, cpdu, clen, now) != 0) {
                    fprintf(stderr, "DECNETD-E-BIND, could not send CTERM Bind\n");
                    rc = 1; done = 1;
                }
                break;
            case DNET_LINK_EV_DATA: {
                enum dnet_cterm_event cev = DNET_CTERM_EV_NONE;
                if (dnet_cterm_rx(&term, eng->rx_data, eng->rx_datalen, &cev)
                        != DNET_CTERM_OK)
                    break;
                if (cev == DNET_CTERM_EV_BOUND) {
                    log_ts(stdout);
                    printf(" DECNETD-I-BOUND, CTERM terminal session bound on"
                           " circuit %s -- terminal is live\n", eng->circuit);
                    fflush(stdout);
                    /* Advertise our characteristics (VT100-class, 80x24). */
                    if (dnet_cterm_send_characteristics(&term, 4, 80, 24,
                            DNET_CTERM_CH_ECHO | DNET_CTERM_CH_WRAP,
                            cpdu, sizeof(cpdu), &clen) == 0)
                        cterm_link_send(eng, sock, ifindex, cpdu, clen, now);
                    /* Put the LOCAL terminal into raw mode (save first). On a
                     * pipe/redirect (no tty) this is a no-op -- the byte pump
                     * still works. */
                    if (isatty(STDIN_FILENO) &&
                        tcgetattr(STDIN_FILENO, &g_saved_tio) == 0) {
                        struct termios raw = g_saved_tio;
                        cfmakeraw(&raw);
                        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
                            g_tio_saved = 1;
                            raw_on = 1;
                        }
                    }
                } else if (cev == DNET_CTERM_EV_WRITE) {
                    if (term.last.datalen) {
                        ssize_t w = write(STDOUT_FILENO, term.last.data,
                                          term.last.datalen);
                        (void)w;
                    }
                } else if (cev == DNET_CTERM_EV_UNBOUND) {
                    log_ts(stdout);
                    printf(" DECNETD-I-UNBOUND, host released the terminal"
                           " session on circuit %s\n", eng->circuit);
                    fflush(stdout);
                    done = 1;
                }
                break;
            }
            case DNET_LINK_EV_DISCONNECT:
            case DNET_LINK_EV_DISCONNECT_CONF:
                log_ts(stdout);
                printf(" DECNETD-I-LINKDOWN, logical link closed on circuit %s\n",
                       eng->circuit);
                fflush(stdout);
                eng->link_active = 0;
                done = 1;
                break;
            default:
                break;
            }
        }

        if (nfd > 1 && (pfd[1].revents & (POLLIN | POLLHUP))) {
            uint8_t inbuf[DNET_CTERM_MAX_DATA];
            ssize_t rn = read(STDIN_FILENO, inbuf, sizeof(inbuf));
            if (rn > 0) {
                /* Real keystrokes -> CTERM Read Data (terminator CR). */
                if (dnet_cterm_read_data(&term, inbuf, (size_t)rn, 0x0d,
                                         cpdu, sizeof(cpdu), &clen) == 0)
                    cterm_link_send(eng, sock, ifindex, cpdu, clen, now);
            } else {
                /* Local EOF: stop soliciting input but KEEP the link open so the
                 * host's remaining output drains. The session ends on the host's
                 * Unbind, a link drop, or --duration. */
                stdin_eof = 1;
                log_ts(stdout);
                printf(" DECNETD-I-EOF, local input closed -- draining remote"
                       " output on circuit %s\n", eng->circuit);
                fflush(stdout);
            }
        }
    }

    if (raw_on)
        restore_local_tty();

    /* On our way out, release the session + link cleanly if still up. */
    if (dnet_cterm_is_bound(&term)) {
        if (dnet_cterm_unbind(&term, DNET_CTERM_UNBIND_NORMAL,
                              cpdu, sizeof(cpdu), &clen) == 0)
            cterm_link_send(eng, sock, ifindex, cpdu, clen, monotonic_sec());
    }
    if (eng->link_active &&
        dnet_link_state_of(&eng->link) != DNET_LINK_CLOSED) {
        size_t dl = 0;
        if (dnet_engine_link_close(eng, DNET_LINK_REASON_NORMAL, frame,
                                   sizeof(frame), &dl, monotonic_sec())
                == DNET_ENGINE_OK) {
            uint8_t dst[DNET_ADDR_LEN];
            memcpy(dst, frame, DNET_ADDR_LEN);
            scs_datalink_send(sock, (int)ifindex, DNET_ETHERTYPE, dst, frame, dl);
        }
    }
    log_ts(stdout);
    printf(" DECNETD-I-SETHOSTEND, SET HOST session ended: cterm writes_recv=%lu"
           " reads_sent=%lu on circuit %s\n",
           term.writes_recv, term.reads_sent, eng->circuit);
    fflush(stdout);
    return rc;
}

/* Split a login-command string into an argv (in place; whitespace-separated, no
 * quoting). Returns the token count. */
static int tokenize_cmd(char *s, char **argv, int max)
{
    int n = 0;
    char *p = s;
    while (*p && n < max - 1) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
            *p++ = '\0';
    }
    argv[n] = NULL;
    return n;
}

/*
 * --cterm-server : the CTERM HOST. Accepts an inbound logical link to the CTERM
 * object (42), spawns a REAL PTY + login-command, and bridges the pty to the
 * CTERM read/write/OOB messages -- real program output -> CTERM Write, inbound
 * CTERM Read Data -> the pty master. It poll()s the pty master AND the datalink
 * once the child is up. The login-command defaults to `vmsdcl --login` (the
 * booted-env target); the hermetic dev-test overrides it with a stand-in
 * interactive program to prove the PTY<->CTERM pump end-to-end.
 */
static int run_cterm_server_loop(struct dnet_engine *eng, int sock,
                                 unsigned ifindex, const char *login_command)
{
    char cmdbuf[512];
    char *cargv[32];
    snprintf(cmdbuf, sizeof(cmdbuf), "%s", login_command);
    if (tokenize_cmd(cmdbuf, cargv, (int)(sizeof(cargv) / sizeof(cargv[0]))) < 1) {
        fprintf(stderr, "DECNETD-E-NOCMD, --login-command is empty\n");
        return 1;
    }

    struct dnet_cterm_session host;
    if (dnet_cterm_session_init(&host, DNET_CTERM_ROLE_HOST) != 0) {
        fprintf(stderr, "DECNETD-E-CTERMINIT, host session init failed\n");
        return 1;
    }

    log_ts(stdout);
    printf(" DECNETD-I-CTERMSRV, awaiting an inbound $ SET HOST to CTERM object"
           " %d on circuit %s (login-command: %s)\n",
           DNET_CTERM_OBJECT, eng->circuit, login_command);
    fflush(stdout);

    uint8_t rxbuf[DNET_FRAME_MAX], cpdu[DNET_CTERM_MAX_PDU];
    size_t clen = 0;
    int master_fd = -1;
    pid_t child = -1;
    int done = 0, rc = 0;
    uint16_t pty_cols = 80, pty_rows = 24;   /* until CHARACTERISTICS arrive */

    while (!g_stop && !done) {
        dnet_tick_t now = monotonic_sec();
        dnet_periodic(eng, sock, ifindex, now);

        /* Only drain the pty once the CTERM session is BOUND: a login-command
         * prints its first prompt the instant it is spawned (before the client's
         * Bind arrives), and CTERM Write is a BOUND-only operation. The kernel
         * pty buffer holds that early output until we are allowed to ship it, so
         * nothing the program wrote is lost. */
        int poll_master = (master_fd >= 0 && dnet_cterm_is_bound(&host));
        struct pollfd pfd[2];
        int nfd = 0;
        pfd[nfd].fd = sock; pfd[nfd].events = POLLIN; pfd[nfd].revents = 0; nfd++;
        if (poll_master) {
            pfd[nfd].fd = master_fd; pfd[nfd].events = POLLIN; pfd[nfd].revents = 0; nfd++;
        }
        int pr = poll(pfd, (nfds_t)nfd, 1000);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "DECNETD-E-POLL, poll failed: %s\n", strerror(errno));
            rc = 1;
            break;
        }

        /* Reap the child if it exited; the pty EOF below finalises the session. */
        if (child > 0 && waitpid(child, NULL, WNOHANG) == child)
            child = -1;

        if (pr > 0 && (pfd[0].revents & POLLIN)) {
            int ev = dnet_recv_route(eng, sock, ifindex, now, rxbuf, sizeof(rxbuf));
            if (ev < 0) {
                fprintf(stderr, "DECNETD-E-RECVFAIL, recv failed: %s\n",
                        strerror(errno));
                rc = 1;
                break;
            }
            switch (ev) {
            case DNET_LINK_EV_CONNECT_IND: {
                char pbuf[8];
                uint16_t pn = eng->link.remote_node;
                int obj = dnet_cterm_sc_connect_object(eng->link.conn_data,
                                                       eng->link.conn_len);
                log_ts(stdout);
                printf(" DECNETD-I-CONNIN, inbound Connect Initiate from %s to"
                       " object %d on circuit %s\n",
                       dnet_addr_str(pn, pbuf, sizeof(pbuf)), obj, eng->circuit);
                fflush(stdout);
                if (obj != DNET_CTERM_OBJECT || master_fd >= 0) {
                    /* Not the CTERM object (or we already have a session):
                     * do not accept -- one active link per instance (scope). */
                    break;
                }
                /* Accept the link (CC), then spawn the PTY + login-command. */
                uint8_t frame[DNET_FRAME_MAX];
                size_t alen = 0;
                if (dnet_engine_link_accept(eng, 0x2002, frame, sizeof(frame),
                                            &alen, now) != DNET_ENGINE_OK)
                    break;
                {
                    uint8_t dst[DNET_ADDR_LEN];
                    memcpy(dst, frame, DNET_ADDR_LEN);
                    scs_datalink_send(sock, (int)ifindex, DNET_ETHERTYPE, dst, frame, alen);
                }
                struct winsize ws;
                memset(&ws, 0, sizeof(ws));
                ws.ws_col = pty_cols;
                ws.ws_row = pty_rows;
                int slave_fd = -1;
                if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) < 0) {
                    fprintf(stderr, "DECNETD-E-PTYOPEN, openpty failed: %s\n",
                            strerror(errno));
                    master_fd = -1;
                    break;
                }
                child = fork();
                if (child < 0) {
                    fprintf(stderr, "DECNETD-E-FORK, fork failed: %s\n",
                            strerror(errno));
                    close(master_fd); close(slave_fd);
                    master_fd = -1;
                    break;
                }
                if (child == 0) {
                    /* Child: the PTY slave becomes the controlling terminal and
                     * stdio; then exec the real login-command. */
                    close(master_fd);
                    close(sock);
                    if (setsid() < 0)
                        _exit(127);
                    if (ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
                        /* non-fatal on some kernels */
                    }
                    dup2(slave_fd, STDIN_FILENO);
                    dup2(slave_fd, STDOUT_FILENO);
                    dup2(slave_fd, STDERR_FILENO);
                    if (slave_fd > STDERR_FILENO)
                        close(slave_fd);
                    execvp(cargv[0], cargv);
                    _exit(127);   /* exec failed */
                }
                /* Parent keeps the master; drop the slave. */
                close(slave_fd);
                log_ts(stdout);
                printf(" DECNETD-I-LINKUP, logical link with %s is RUN --"
                       " login-command spawned (pid %ld) on circuit %s\n",
                       dnet_addr_str(pn, pbuf, sizeof(pbuf)),
                       (long)child, eng->circuit);
                fflush(stdout);
                break;
            }
            case DNET_LINK_EV_DATA: {
                enum dnet_cterm_event cev = DNET_CTERM_EV_NONE;
                if (dnet_cterm_rx(&host, eng->rx_data, eng->rx_datalen, &cev)
                        != DNET_CTERM_OK)
                    break;
                if (cev == DNET_CTERM_EV_BIND_IND) {
                    if (dnet_cterm_bind_accept(&host, "OVMX", cpdu, sizeof(cpdu),
                                               &clen) == 0)
                        cterm_link_send(eng, sock, ifindex, cpdu, clen, now);
                    log_ts(stdout);
                    printf(" DECNETD-I-BOUND, CTERM session bound (terminal %s)"
                           " on circuit %s\n", host.peer_name, eng->circuit);
                    fflush(stdout);
                } else if (cev == DNET_CTERM_EV_CHARACTERISTICS) {
                    pty_cols = host.width ? host.width : pty_cols;
                    pty_rows = host.page ? host.page : pty_rows;
                    if (master_fd >= 0) {
                        struct winsize ws;
                        memset(&ws, 0, sizeof(ws));
                        ws.ws_col = pty_cols;
                        ws.ws_row = pty_rows;
                        ioctl(master_fd, TIOCSWINSZ, &ws);
                    }
                } else if (cev == DNET_CTERM_EV_READ_DATA) {
                    if (master_fd >= 0 && host.last.datalen) {
                        ssize_t w = write(master_fd, host.last.data,
                                          host.last.datalen);
                        (void)w;
                    }
                } else if (cev == DNET_CTERM_EV_OOB) {
                    if (master_fd >= 0) {
                        uint8_t oc = host.last.oob_char;
                        ssize_t w = write(master_fd, &oc, 1);
                        (void)w;
                    }
                } else if (cev == DNET_CTERM_EV_UNBOUND) {
                    done = 1;
                }
                break;
            }
            case DNET_LINK_EV_DISCONNECT:
            case DNET_LINK_EV_DISCONNECT_CONF:
                log_ts(stdout);
                printf(" DECNETD-I-LINKDOWN, peer disconnected on circuit %s\n",
                       eng->circuit);
                fflush(stdout);
                eng->link_active = 0;
                done = 1;
                break;
            default:
                break;
            }
        }

        /* PTY master readable -> real program output -> CTERM Write. */
        if (poll_master && nfd > 1 &&
            (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            uint8_t obuf[DNET_CTERM_MAX_DATA];
            ssize_t on = read(master_fd, obuf, sizeof(obuf));
            if (on > 0) {
                if (dnet_cterm_write(&host, obuf, (size_t)on,
                                     DNET_CTERM_WR_NOFORMAT,
                                     cpdu, sizeof(cpdu), &clen) == 0)
                    cterm_link_send(eng, sock, ifindex, cpdu, clen, now);
            } else {
                /* pty EOF: the login-command exited. Unbind + tear the link. */
                log_ts(stdout);
                printf(" DECNETD-I-CHILDEXIT, login-command exited -- unbinding"
                       " the terminal session on circuit %s\n", eng->circuit);
                fflush(stdout);
                if (dnet_cterm_is_bound(&host) &&
                    dnet_cterm_unbind(&host, DNET_CTERM_UNBIND_NORMAL,
                                      cpdu, sizeof(cpdu), &clen) == 0)
                    cterm_link_send(eng, sock, ifindex, cpdu, clen, now);
                close(master_fd);
                master_fd = -1;
                size_t dl = 0;
                uint8_t frame[DNET_FRAME_MAX];
                if (eng->link_active &&
                    dnet_engine_link_close(eng, DNET_LINK_REASON_NORMAL, frame,
                                           sizeof(frame), &dl, now) == DNET_ENGINE_OK) {
                    uint8_t dst[DNET_ADDR_LEN];
                    memcpy(dst, frame, DNET_ADDR_LEN);
                    scs_datalink_send(sock, (int)ifindex, DNET_ETHERTYPE, dst, frame, dl);
                }
                done = 1;
            }
        }
    }

    if (master_fd >= 0)
        close(master_fd);
    if (child > 0) {
        kill(child, SIGHUP);
        waitpid(child, NULL, 0);
    }
    log_ts(stdout);
    printf(" DECNETD-I-CTERMSRVEND, CTERM host session ended: writes_sent=%lu"
           " reads_recv=%lu on circuit %s\n",
           host.writes_sent, host.reads_recv, eng->circuit);
    fflush(stdout);
    return rc;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s --address AREA.NODE [options]\n"
        "  --address A.N       DECnet Phase IV executor address (REQUIRED;\n"
        "                      1..63 . 1..1023). No identity is invented if\n"
        "                      omitted -- the daemon exits (INV-6).\n"
        "  --name NAME         NCP node name (1..6 chars; default OVMX)\n"
        "  --iface IFNAME      datalink interface (default %s)\n"
        "  --device DEV        VMS device label for the circuit (default EWA0)\n"
        "  --circuit CIRC      DECnet circuit name (default derived, e.g. EWA-0)\n"
        "  --hello-interval N  HELLO cadence T3 seconds (default %u, oracle vms-3be)\n"
        "  --duration N        run N seconds then exit (default: until SIGINT/TERM)\n"
        "  --show-executor     print the NCP executor summary and exit (no socket)\n"
        "  --self-test         run the in-process tx/rx/adjacency proof and exit\n"
        "                      (no CAP_NET_RAW, no netdev -- moves a real HELLO\n"
        "                      frame over a socketpair; DECnet analogue of\n"
        "                      scsd --dlm-selftest)\n"
        "  --nsp-selftest      run the NSP logical-link connection proof and exit\n"
        "                      (no CAP_NET_RAW -- two engines OPEN a link, move a\n"
        "                      data segment+ack, and DISCONNECT over a socketpair)\n"
        "  --set-host-selftest run the $ SET HOST / CTERM terminal-service proof\n"
        "                      and exit (no CAP_NET_RAW -- two engines carry a\n"
        "                      whole terminal session: Bind, characteristics,\n"
        "                      screen output, keystrokes, out-of-band, Unbind,\n"
        "                      over a socketpair; every payload byte-identical)\n"
        "  --connect A.N       CLIENT: open an NSP logical link to A.N over the\n"
        "                      LIVE datalink (Connect Initiate; needs CAP_NET_RAW)\n"
        "  --connect-data STR  with --connect: send STR as one data segment once\n"
        "                      the link is RUN, then disconnect cleanly\n"
        "  --listen            SERVER: accept an inbound Connect Initiate on the\n"
        "                      live datalink and deliver its data segments\n"
        "  --set-host A.N      $ SET HOST client: open a CTERM terminal session to\n"
        "                      A.N's CTERM object over the LIVE datalink and bridge\n"
        "                      the LOCAL terminal to it (real stdin/stdout; needs\n"
        "                      CAP_NET_RAW)\n"
        "  --user NAME         with --set-host: CTERM access-control username\n"
        "                      (SET HOST/USERNAME= analog; default SYSTEM; never\n"
        "                      read from $USER -- vms-cb5 identity census)\n"
        "  --cterm-server      CTERM HOST: accept an inbound $ SET HOST, spawn a\n"
        "                      real PTY + login-command, and bridge it to the CTERM\n"
        "                      read/write messages (needs CAP_NET_RAW)\n"
        "  --login-command CMD with --cterm-server: the interactive program to run\n"
        "                      as the session (default \"vmsdcl --login\"; the dev\n"
        "                      bracket overrides it with a stand-in program)\n"
        "  --object N          task/object number (logged; routing is a later rung)\n",
        argv0, DECNETD_DEFAULT_IFACE, (unsigned)DNET_T3_DEFAULT);
}

int main(int argc, char **argv)
{
    const char *ifname = DECNETD_DEFAULT_IFACE;
    const char *addr_s = NULL;
    const char *name = "OVMX";
    const char *device = "EWA0";
    const char *circuit = NULL;
    int hello_interval = (int)DNET_T3_DEFAULT;
    int duration = 0;
    int show_executor_only = 0;
    int self_test = 0;
    int nsp_self_test = 0;
    int sethost_self_test = 0;
    const char *connect_to = NULL;     /* --connect A.N : open a logical link  */
    const char *connect_data = NULL;   /* --connect-data STR : one data segment */
    int listen_mode = 0;               /* --listen : accept an inbound link     */
    int object_num = 0;                /* --object N : task/object number (log)  */
    const char *set_host_to = NULL;    /* --set-host A.N : CTERM terminal client */
    const char *set_host_user = "SYSTEM"; /* --user : CTERM access-control name   */
    int cterm_server = 0;              /* --cterm-server : CTERM host w/ real PTY */
    const char *login_command = "vmsdcl --login"; /* --login-command CMD         */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--address") && i + 1 < argc)      addr_s = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc)    name = argv[++i];
        else if (!strcmp(argv[i], "--iface") && i + 1 < argc)   ifname = argv[++i];
        else if (!strcmp(argv[i], "--device") && i + 1 < argc)  device = argv[++i];
        else if (!strcmp(argv[i], "--circuit") && i + 1 < argc) circuit = argv[++i];
        else if (!strcmp(argv[i], "--hello-interval") && i + 1 < argc)
            hello_interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--duration") && i + 1 < argc)
            duration = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--show-executor")) show_executor_only = 1;
        else if (!strcmp(argv[i], "--self-test"))     self_test = 1;
        else if (!strcmp(argv[i], "--nsp-selftest"))  nsp_self_test = 1;
        else if (!strcmp(argv[i], "--set-host-selftest")) sethost_self_test = 1;
        else if (!strcmp(argv[i], "--connect") && i + 1 < argc)      connect_to = argv[++i];
        else if (!strcmp(argv[i], "--connect-data") && i + 1 < argc) connect_data = argv[++i];
        else if (!strcmp(argv[i], "--listen"))                       listen_mode = 1;
        else if (!strcmp(argv[i], "--set-host") && i + 1 < argc)     set_host_to = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc)         set_host_user = argv[++i];
        else if (!strcmp(argv[i], "--cterm-server"))                 cterm_server = 1;
        else if (!strcmp(argv[i], "--login-command") && i + 1 < argc) login_command = argv[++i];
        else if (!strcmp(argv[i], "--object") && i + 1 < argc)       object_num = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "DECNETD-E-BADARG, unknown argument '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (self_test)
        return run_self_test();
    if (nsp_self_test)
        return run_nsp_selftest();
    if (sethost_self_test)
        return run_sethost_selftest();

    /* Identity is required and never invented (INV-6; the scsd
     * resolve_node_identity discipline: a wrong identity must never be made up). */
    unsigned area = 0, node = 0;
    if (!addr_s || parse_addr(addr_s, &area, &node) != 0) {
        fprintf(stderr, "DECNETD-E-NOADDRESS, a valid --address AREA.NODE is"
                        " required (1..63 . 1..1023); refusing to invent an"
                        " executor address\n");
        return 1;
    }

    if (hello_interval < 1)
        hello_interval = (int)DNET_T3_DEFAULT;

    /* --show-executor: report the identity/circuit this endnode would adopt and
     * exit, opening NO socket (needs no privilege). Analogue of scsd
     * --show-identity. */
    if (show_executor_only) {
        struct dnet_engine e;
        uint8_t zmac[6] = {0};
        if (dnet_engine_init(&e, area, node, name, device, circuit, zmac,
                             (uint16_t)hello_interval, 0, monotonic_sec()) != 0) {
            fprintf(stderr, "DECNETD-E-INIT, engine init failed\n");
            return 1;
        }
        dnet_engine_show_executor(&e, stdout);
        dnet_engine_show_circuit(&e, stdout);
        return 0;
    }

    /* Open the raw-L2 datalink (INV-6 fail-honest: no per-process fake if the
     * netdev cannot be opened). Same abstraction scsd.c uses. */
    int sock = scs_datalink_open(ifname, DNET_ETHERTYPE);
    if (sock < 0) {
        fprintf(stderr,
                "DECNETD-E-NOSOCKET, scs_datalink_open('%s', 0x%04x) failed: %s\n"
                "  (Linux needs CAP_NET_RAW -- run as root or"
                " setcap cap_net_raw+ep on this binary; the interface must exist"
                " and share the Phase IV L2 segment)\n",
                ifname, (unsigned)DNET_ETHERTYPE, strerror(errno));
        return 1;
    }
    unsigned ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        fprintf(stderr, "DECNETD-E-NOIFACE, unknown interface '%s': %s\n",
                ifname, strerror(errno));
        scs_datalink_close(sock);
        return 1;
    }
    uint8_t hw_mac[6] = {0};
    if (scs_datalink_get_hwaddr(ifname, hw_mac) != 0) {
        fprintf(stderr, "DECNETD-E-NOHWADDR, cannot read HW address of '%s': %s\n",
                ifname, strerror(errno));
        scs_datalink_close(sock);
        return 1;
    }
    /* Wake the receive loop about once a second so the T3 cadence and the
     * listen-timer sweep still fire on an idle wire (as scsd does). */
    if (scs_datalink_set_recv_timeout(sock, 1) < 0) {
        fprintf(stderr, "DECNETD-E-RCVTIMEO, set_recv_timeout failed: %s\n",
                strerror(errno));
        scs_datalink_close(sock);
        return 1;
    }
    /* Receive unicast NSP frames addressed to our DECnet MAC. A faithful node
     * owning its NIC would program that MAC onto the interface; on a shared
     * bridge OVMX does not own, we go promiscuous and dst-filter in software
     * (see the loop). Best-effort: HELLO/adjacency still works multicast if
     * this is refused, so a failure is a warning, not fatal. */
    if (scs_datalink_set_promisc(sock, ifname) != 0) {
        log_ts(stdout);
        printf(" DECNETD-W-NOPROMISC, could not enable promiscuous reception on"
               " %s (%s) -- inbound unicast NSP may not be received\n",
               ifname, strerror(errno));
        fflush(stdout);
    }

    struct dnet_engine eng;
    if (dnet_engine_init(&eng, area, node, name, device, circuit, hw_mac,
                         (uint16_t)hello_interval, 0, monotonic_sec()) != 0) {
        fprintf(stderr, "DECNETD-E-INIT, engine init failed\n");
        scs_datalink_close(sock);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (duration > 0) {
        signal(SIGALRM, on_signal);
        alarm((unsigned)duration);
    }

    /* Startup: the VMS-visible face (never the raw socket). */
    log_ts(stdout);
    printf(" DECNETD-I-STARTED, DECnet Phase IV endnode up on circuit %s"
           " (datalink hidden behind the VMS surface, Rule 1)\n", eng.circuit);
    dnet_engine_show_executor(&eng, stdout);
    dnet_engine_show_circuit(&eng, stdout);
    fflush(stdout);

    /* --- $ SET HOST / CTERM interactive session over the LIVE datalink -------
     * (rd vms-aac0 live bracket, child of vms-30e / vms-4d2). These modes carry
     * a REAL PTY + login-command (host) and real stdin/stdout (client), poll()ing
     * both the datalink and the aux fd. They own their own loop and do not fall
     * through to the routing/NSP-data loop below. */
    if (set_host_to) {
        int r = run_set_host_loop(&eng, sock, ifindex, set_host_to, set_host_user);
        scs_datalink_close(sock);
        return r;
    }
    if (cterm_server) {
        int r = run_cterm_server_loop(&eng, sock, ifindex, login_command);
        scs_datalink_close(sock);
        return r;
    }

    uint8_t frame[DNET_FRAME_MAX];
    uint8_t rxbuf[DNET_FRAME_MAX];

    /* --- NSP logical-link mode over the LIVE datalink (rd vms-c23) ----------
     * The socketpair selftest proves the FSM in isolation; here the SAME FSM is
     * driven with every PDU crossing the real AF_PACKET datalink. Client
     * (--connect A.N) opens a link, optionally sends one data segment, then
     * disconnects; server (--listen) accepts an inbound Connect Initiate,
     * delivers data, and mirrors the teardown. */
    enum { CL_NONE, CL_CONNECTING, CL_UP, CL_SENT, CL_CLOSING } cl = CL_NONE;
    unsigned peer_area = 0, peer_node = 0;
    if (connect_to) {
        if (parse_addr(connect_to, &peer_area, &peer_node) != 0) {
            fprintf(stderr, "DECNETD-E-BADPEER, --connect wants AREA.NODE"
                            " (1..63 . 1..1023)\n");
            scs_datalink_close(sock);
            return 1;
        }
        size_t flen = 0;
        const uint8_t *cd = connect_data ? (const uint8_t *)connect_data : NULL;
        size_t cdl = connect_data ? strlen(connect_data) : 0;
        if (dnet_engine_link_open(&eng, peer_area, peer_node, 0x2001, cd, cdl,
                                  1459, 1, DNET_NSP_VER_41,
                                  frame, sizeof(frame), &flen, monotonic_sec())
                == DNET_ENGINE_OK) {
            uint8_t peer_id[DNET_ADDR_LEN];
            dnet_id_from_addr(peer_area, peer_node, peer_id);
            scs_datalink_send(sock, (int)ifindex, DNET_ETHERTYPE, peer_id, frame, flen);
            cl = CL_CONNECTING;
            log_ts(stdout);
            printf(" DECNETD-I-CONNECTING, Connect Initiate sent to %u.%u"
                   " (object %d) on circuit %s\n",
                   peer_area, peer_node, object_num, eng.circuit);
            fflush(stdout);
        } else {
            fprintf(stderr, "DECNETD-E-NOCONNECT, could not open a logical link"
                            " to %u.%u\n", peer_area, peer_node);
            scs_datalink_close(sock);
            return 1;
        }
    }
    if (listen_mode) {
        log_ts(stdout);
        printf(" DECNETD-I-LISTEN, awaiting an inbound Connect Initiate"
               " (object %d) on circuit %s\n", object_num, eng.circuit);
        fflush(stdout);
    }

    while (!g_stop) {
        dnet_tick_t now = monotonic_sec();

        /* T3 emission cadence: build + transmit our endnode HELLO. */
        if (dnet_engine_hello_due(&eng, now)) {
            size_t flen = 0;
            if (dnet_engine_build_hello_frame(&eng, frame, sizeof(frame), &flen)
                    == DNET_ENGINE_OK) {
                ssize_t sent = scs_datalink_send(sock, (int)ifindex,
                                                 DNET_ETHERTYPE, DNET_HELLO_MCAST,
                                                 frame, flen);
                if (sent < 0) {
                    fprintf(stderr, "DECNETD-E-SENDFAIL, HELLO transmit failed: %s\n",
                            strerror(errno));
                } else {
                    dnet_engine_hello_emitted(&eng, now);
                    log_ts(stdout);
                    printf(" DECNETD-I-HELLOSENT, circuit %s seq=%lu bytes=%zd\n",
                           eng.circuit, eng.hello_sent, sent);
                    fflush(stdout);
                }
            }
        }

        /* Age out adjacencies whose listen timer lapsed. */
        int gone = dnet_engine_tick(&eng, now);
        if (gone > 0) {
            log_ts(stdout);
            printf(" DECNETD-I-ADJDOWN, %d adjacency(ies) timed out on circuit %s\n",
                   gone, eng.circuit);
            fflush(stdout);
        }

        /* NSP: drive the active link's Connect-Initiate retransmit / give-up. */
        if (eng.link_active) {
            size_t tlen = 0;
            int thas = 0;
            if (dnet_engine_link_tick(&eng, now, frame, sizeof(frame), &tlen, &thas)
                    == DNET_ENGINE_OK && thas) {
                uint8_t dst[DNET_ADDR_LEN];
                memcpy(dst, frame, DNET_ADDR_LEN);
                scs_datalink_send(sock, (int)ifindex, DNET_ETHERTYPE, dst, frame, tlen);
                log_ts(stdout);
                printf(" DECNETD-I-CIRETRANS, Connect Initiate retransmitted on"
                       " circuit %s\n", eng.circuit);
                fflush(stdout);
            }
            /* CI give-up (max retransmits) closes the link as unreachable. */
            if (cl == CL_CONNECTING && dnet_link_state_of(&eng.link) == DNET_LINK_CLOSED) {
                log_ts(stdout);
                printf(" DECNETD-W-UNREACH, peer %u.%u did not answer Connect"
                       " Initiate -- link abandoned\n", peer_area, peer_node);
                fflush(stdout);
                eng.link_active = 0;
                cl = CL_NONE;
                g_stop = 1;
            }
        }

        ssize_t n = scs_datalink_recv(sock, rxbuf, sizeof(rxbuf));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue; /* timer wakeup / signal -- re-check cadence */
            fprintf(stderr, "DECNETD-E-RECVFAIL, recv failed: %s\n", strerror(errno));
            break;
        }
        /* Drop our own echo (an AF_PACKET SOCK_RAW socket sees frames it sent). */
        if ((size_t)n >= DNET_ETH_HDRLEN &&
            memcmp(rxbuf + 6, eng.my_id, DNET_ADDR_LEN) == 0)
            continue;

        /* Demux by the routing flag: a long-data frame carries an NSP PDU for
         * the logical-link service; everything else goes to the HELLO/adjacency
         * path. */
        int is_nsp = ((size_t)n > (size_t)DNET_ETH_HDRLEN + DNET_DATA_LENPREFIX) &&
                     rxbuf[DNET_ETH_HDRLEN + DNET_DATA_LENPREFIX] == DNET_RFLAG_LONG_DATA;
        if (is_nsp) {
            /* Promiscuous mode also surfaces unicast NSP addressed to OTHER
             * nodes -- process only frames whose Ethernet destination is our
             * own DECnet id (a real MAC-owning node's NIC would filter this). */
            if (memcmp(rxbuf, eng.my_id, DNET_ADDR_LEN) != 0)
                continue;
            size_t rlen = 0;
            int has_reply = 0;
            enum dnet_link_event ev = DNET_LINK_EV_NONE;
            if (dnet_engine_link_rx(&eng, now, rxbuf, (size_t)n, frame, sizeof(frame),
                                    &rlen, &has_reply, &ev) != DNET_ENGINE_OK)
                continue;
            if (has_reply) {                    /* data ACK or Disconnect Confirm */
                uint8_t dst[DNET_ADDR_LEN];
                memcpy(dst, frame, DNET_ADDR_LEN);
                scs_datalink_send(sock, (int)ifindex, DNET_ETHERTYPE, dst, frame, rlen);
            }
            char pbuf[8];
            uint16_t pn = eng.link.remote_node;
            switch (ev) {
            case DNET_LINK_EV_CONNECT_IND:
                log_ts(stdout);
                printf(" DECNETD-I-CONNIN, inbound Connect Initiate from %s on"
                       " circuit %s%s\n", dnet_addr_str(pn, pbuf, sizeof(pbuf)),
                       eng.circuit, listen_mode ? " -- accepting" : " -- no listener, ignoring");
                fflush(stdout);
                if (listen_mode) {
                    size_t clen = 0;
                    if (dnet_engine_link_accept(&eng, 0x2002, frame, sizeof(frame),
                                                &clen, now) == DNET_ENGINE_OK) {
                        uint8_t dst[DNET_ADDR_LEN];
                        memcpy(dst, frame, DNET_ADDR_LEN);
                        scs_datalink_send(sock, (int)ifindex, DNET_ETHERTYPE, dst, frame, clen);
                        log_ts(stdout);
                        printf(" DECNETD-I-LINKUP, logical link with %s is RUN on"
                               " circuit %s\n", dnet_addr_str(pn, pbuf, sizeof(pbuf)),
                               eng.circuit);
                        fflush(stdout);
                    }
                }
                break;
            case DNET_LINK_EV_CONNECT_CONF:
                cl = CL_UP;
                log_ts(stdout);
                printf(" DECNETD-I-LINKUP, logical link to %u.%u is RUN on circuit %s\n",
                       peer_area, peer_node, eng.circuit);
                fflush(stdout);
                if (connect_data) {             /* client: send one data segment */
                    size_t sl = 0;
                    if (dnet_engine_link_send(&eng, (const uint8_t *)connect_data,
                                              strlen(connect_data), frame, sizeof(frame),
                                              &sl, now) == DNET_ENGINE_OK) {
                        uint8_t dst[DNET_ADDR_LEN];
                        memcpy(dst, frame, DNET_ADDR_LEN);
                        scs_datalink_send(sock, (int)ifindex, DNET_ETHERTYPE, dst, frame, sl);
                        cl = CL_SENT;
                        log_ts(stdout);
                        printf(" DECNETD-I-DATATX, sent %zu byte(s) on circuit %s\n",
                               strlen(connect_data), eng.circuit);
                        fflush(stdout);
                    }
                } else {                        /* client: nothing to send, close */
                    size_t dl = 0;
                    if (dnet_engine_link_close(&eng, DNET_LINK_REASON_NORMAL, frame,
                                               sizeof(frame), &dl, now) == DNET_ENGINE_OK) {
                        uint8_t dst[DNET_ADDR_LEN];
                        memcpy(dst, frame, DNET_ADDR_LEN);
                        scs_datalink_send(sock, (int)ifindex, DNET_ETHERTYPE, dst, frame, dl);
                        cl = CL_CLOSING;
                    }
                }
                break;
            case DNET_LINK_EV_DATA:
                log_ts(stdout);
                printf(" DECNETD-I-DATARX, %u byte(s) delivered on circuit %s: \"%.*s\"\n",
                       (unsigned)eng.rx_datalen, eng.circuit,
                       (int)eng.rx_datalen, (const char *)eng.rx_data);
                fflush(stdout);
                break;
            case DNET_LINK_EV_ACK:
                if (cl == CL_SENT) {            /* client: data acked -> disconnect */
                    size_t dl = 0;
                    if (dnet_engine_link_close(&eng, DNET_LINK_REASON_NORMAL, frame,
                                               sizeof(frame), &dl, now) == DNET_ENGINE_OK) {
                        uint8_t dst[DNET_ADDR_LEN];
                        memcpy(dst, frame, DNET_ADDR_LEN);
                        scs_datalink_send(sock, (int)ifindex, DNET_ETHERTYPE, dst, frame, dl);
                        cl = CL_CLOSING;
                        log_ts(stdout);
                        printf(" DECNETD-I-DISCONN, data acknowledged -- disconnecting\n");
                        fflush(stdout);
                    }
                }
                break;
            case DNET_LINK_EV_DISCONNECT:
                log_ts(stdout);
                printf(" DECNETD-I-LINKDOWN, peer disconnected on circuit %s\n", eng.circuit);
                fflush(stdout);
                eng.link_active = 0;
                if (connect_to)                /* the client run is complete */
                    g_stop = 1;
                break;
            case DNET_LINK_EV_DISCONNECT_CONF:
                log_ts(stdout);
                printf(" DECNETD-I-LINKDOWN, disconnect confirmed -- link closed on"
                       " circuit %s\n", eng.circuit);
                fflush(stdout);
                eng.link_active = 0;
                cl = CL_NONE;
                if (connect_to)                /* the client run is complete */
                    g_stop = 1;
                break;
            default:
                break;
            }
            continue;
        }

        char afrom[8];
        uint8_t from[6];
        enum dnet_adj_state st = DNET_ADJ_DOWN;
        int rc = dnet_engine_rx_frame(&eng, now, rxbuf, (size_t)n, from, &st);
        if (rc == 1) {
            uint16_t na = dnet_addr_from_id(from);
            if (st == DNET_ADJ_UP) {
                log_ts(stdout);
                printf(" DECNETD-I-ADJUP, adjacency to %s is up on circuit %s\n",
                       dnet_addr_str(na, afrom, sizeof(afrom)), eng.circuit);
            } else {
                log_ts(stdout);
                printf(" DECNETD-I-ADJINIT, heard %s (%s) on circuit %s\n",
                       dnet_addr_str(na, afrom, sizeof(afrom)),
                       st == DNET_ADJ_INITIALIZING ? "initializing" : "?",
                       eng.circuit);
            }
            fflush(stdout);
        }
    }

    log_ts(stdout);
    printf(" DECNETD-I-STOPPING, shutting down circuit %s\n", eng.circuit);
    dnet_engine_show_adjacent(&eng, stdout);
    printf("DECNETD-I-COUNTERS, hello_sent=%lu hello_recv=%lu frames_recv=%lu"
           " frames_dropped=%lu adj_up=%lu adj_down=%lu nsp_recv=%lu nsp_dropped=%lu\n",
           eng.hello_sent, eng.hello_recv, eng.frames_recv, eng.frames_dropped,
           eng.adj_up_events, eng.adj_down_events,
           eng.nsp_frames_recv, eng.nsp_frames_dropped);
    fflush(stdout);

    scs_datalink_close(sock);
    return 0;
}
