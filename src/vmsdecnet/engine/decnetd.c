/*
 * decnetd.c - the OVMX DECnet NETACP: session control + the device/object
 *             dispatch face, with the Phase IV wire engine as its low-privilege
 *             DATALINK (rd vms-449d engine rung 1; rd vms-9ab P5 NETACP reframe,
 *             design vms-515 §3.3/§3.4; epic vms-30e).
 *
 * THE NETACP MODEL (P5, vms-9ab). This process is DECnet's privileged
 * RUN/DETACHED session-control ACP (JOB_CONTROL's category -- NOT kernel-
 * resident; DECnet has no DLM-survival analogue that would justify moving it
 * into vms.ko). It OWNS the executive-resident faces a VMS program sees: the
 * _NET: device (born in src/kernel-core/vms_devtab.c, $ASSIGN/$GETDVI-able
 * cross-process) and the network-object dispatch (object 42 = CTERM -> RTAn: +
 * $CREPRC LOGINOUT). The wire engine below -- HELLO/adjacency/NSP/CTERM codecs
 * over an AF_PACKET raw-L2 socket -- is DEMOTED to NETACP's DATALINK: it runs at
 * LOW privilege, parses hostile frames, and hands the privileged control path
 * only a VALIDATED TYPED DESCRIPTOR (the A2/A8 seam, see dnet_cterm_host.h and
 * the --isolation-test mode). The privileged path parses no attacker bytes.
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
#include <net/if.h>      /* if_nametoindex() */
#include <poll.h>       /* the --cterm-server loop waits on wire + session */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "dnet_engine.h"
#include "dnet_cterm.h"     /* CTERM terminal-service protocol (--set-host-selftest) */
#include "dnet_cterm_host.h" /* CTERM HOST session: $CREPRC -> LOGINOUT on RTAn: */
#include "ovmx_identity.h"  /* INV-1 identity SSOT: human banner = OVMX product id */
#include "scs_datalink.h"   /* the shared raw-L2 datalink (src/libdatalink) */
#include "ssdef.h"          /* SS$_BADPARAM (isolation-seam refusal, vms-9ab) */
#include "vms_kif.h"        /* $GETDVI readback: the sec-7.5 anti-LARP tell */

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
    /* The connect is built in the ORACLE-OBSERVED shape (docs/oracle/
     * vax-sethost-cterm.pcap frame 5): destination = format-0 object 42,
     * source = format-2 coded descriptor carrying the local user "SYSTEM",
     * and EMPTY access-control fields -- a real SET HOST carries no password.
     * The group/user codes are the specimen's own. */
    if (dnet_cterm_sc_connect_build(DNET_CTERM_OBJECT, "SYSTEM", 0x021a, 0x2020,
                                    "", "", "",
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
 * ================== --cterm-accept-test (rd vms-f40) ==================
 * THE GROUND-SOURCE ACCEPTANCE for "an inbound $ SET HOST reaches an
 * AUTHENTICATED LOGINOUT prompt". It is the design's sec-7.1/7.5 ratification
 * gate in executable form, and it is written so that A FAKE CANNOT PASS IT:
 *
 *   - THE WIRE IS REAL. A client engine opens a genuine NSP logical link to
 *     Session Control OBJECT 42, carrying a connect message built in the
 *     ORACLE-OBSERVED shape (docs/oracle/vax-sethost-cterm.pcap frame 5:
 *     format-0 destination object 42, format-2 source descriptor carrying
 *     "SYSTEM", EMPTY access-control fields). Every frame is encoded and moved
 *     over a real socketpair(2) -- the same transport the landed
 *     --nsp-selftest and --set-host-selftest proofs use -- and every CTERM PDU
 *     rides inside a real NSP data segment.
 *
 *   - THE SESSION IS REAL. The inbound connect is dispatched through
 *     dnet_cterm_host_open(), which mints an RTAn: IN THE EXECUTIVE and calls
 *     $CREPRC PRC$M_INTER|PRC$M_LOGINOUT to create a process running the REAL
 *     SYS$SYSTEM:LOGINOUT.EXE on it. There is no stub login, no scripted
 *     banner and no canned response anywhere in this file: every byte the
 *     client "sees" below came out of that process's terminal.
 *
 *   - THE AUTHENTICATION IS REAL, AND IS PROVEN BY REFUSAL. The test types
 *     credentials that MUST be rejected -- a nonexistent account, then a wrong
 *     password for a real one -- and requires LOGINOUT's own authorization
 *     failure to come back over the link. A no-auth implementation (the hole
 *     this item closes) answers with a "$" prompt instead and fails here.
 *
 *   - THE DEVICE IS REAL, READ FROM ANOTHER PROCESS. While the session is
 *     live, THIS process (which is NOT the session) $GETDVIs the RTAn: name
 *     out of the executive and finds a genuine DC$_TERM row. That is the
 *     design's own anti-LARP tell (sec 7.5): "if a 'SET HOST works' green can
 *     be produced WITHOUT THE EXECUTIVE DEVICE TABLE CHANGING, it is a LARP."
 *
 * WHERE IT RUNS. It needs a real /dev/vms, a real SYSUAF and a real
 * LOGINOUT.EXE, so it runs INSIDE THE BOOTED IMAGE -- the shared acceptance
 * battery invokes it as a DCL foreign command (tests/qemu/lib/
 * dcl_acceptance_battery.sh, "DECnet CTERM (vms-f40)"). It needs NO
 * CAP_NET_RAW and no netdev: the datalink is the socketpair, exactly as the
 * other selftests.
 *
 * INV-6: if the executive is absent or the session cannot be created, this
 * FAILS. It never falls back to a per-process imitation of a login.
 */

/* One inbound-SET-HOST acceptance context: a client engine + CTERM terminal
 * session on one side of a socketpair, the CTERM HOST session (and the real
 * LOGINOUT process behind it) on the other. */
struct ct_accept {
    int      sv[2];
    struct dnet_engine L;                    /* client (SET HOST initiator) */
    struct dnet_engine R;                    /* host node                   */
    struct dnet_cterm_session term;          /* client-side CTERM FSM       */
    struct dnet_cterm_host_session hs;       /* host session + LOGINOUT     */
    char     screen[16384];                  /* what the client has SEEN    */
    size_t   screen_len;
    dnet_tick_t t;
};

static void ct_screen_append(struct ct_accept *c, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n && c->screen_len + 1 < sizeof(c->screen); i++)
        c->screen[c->screen_len++] = (char)d[i];
    c->screen[c->screen_len] = '\0';
}

/* Case-insensitive substring search over the captured screen. */
static int ct_screen_has(const struct ct_accept *c, const char *needle)
{
    size_t nl = strlen(needle);

    if (nl == 0 || c->screen_len < nl)
        return 0;
    for (size_t i = 0; i + nl <= c->screen_len; i++) {
        size_t j = 0;
        while (j < nl) {
            char a = c->screen[i + j], b = needle[j];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (a != b) break;
            j++;
        }
        if (j == nl)
            return 1;
    }
    return 0;
}

/* Ship one CTERM PDU client -> host over the real link, and let the HOST
 * consume it: a Bind is answered by the host FSM, a Read Data is written to
 * the session's terminal (i.e. typed at LOGINOUT). Returns 0 or -1. */
static int ct_to_host(struct ct_accept *c, const uint8_t *pdu, size_t plen)
{
    uint8_t frame[DNET_FRAME_MAX], rxbuf[DNET_FRAME_MAX], reply[DNET_FRAME_MAX];
    size_t flen = 0, rlen = 0, rxlen = 0;
    int has_reply = 0;
    enum dnet_link_event lev = DNET_LINK_EV_NONE;
    enum dnet_cterm_event cev = DNET_CTERM_EV_NONE;

    if (dnet_engine_link_send(&c->L, pdu, plen, frame, sizeof(frame), &flen, c->t) != 0 ||
        move_frame(c->sv[0], c->sv[1], frame, flen, rxbuf, sizeof(rxbuf), &rxlen) != 0 ||
        dnet_engine_link_rx(&c->R, c->t, rxbuf, rxlen, reply, sizeof(reply), &rlen,
                            &has_reply, &lev) != 0 || lev != DNET_LINK_EV_DATA)
        return -1;
    if (dnet_cterm_rx(&c->hs.cterm, c->R.rx_data, c->R.rx_datalen, &cev) != DNET_CTERM_OK)
        return -1;
    if (cev == DNET_CTERM_EV_READ_DATA) {
        /* The remote's keystrokes go to the SESSION's terminal -- to LOGINOUT,
         * which is the thing that decides whether they are a valid login. */
        if (c->hs.cterm.last.datalen &&
            dnet_cterm_host_write(&c->hs, c->hs.cterm.last.data,
                                  c->hs.cterm.last.datalen) < 0)
            return -1;
        if (c->hs.cterm.last.terminator) {
            uint8_t nl = c->hs.cterm.last.terminator;
            if (dnet_cterm_host_write(&c->hs, &nl, 1) < 0)
                return -1;
        }
    }
    if (has_reply) {
        if (move_frame(c->sv[1], c->sv[0], reply, rlen, rxbuf, sizeof(rxbuf), &rxlen) != 0 ||
            dnet_engine_link_rx(&c->L, c->t, rxbuf, rxlen, frame, sizeof(frame), &flen,
                                &has_reply, &lev) != 0)
            return -1;
    }
    c->t++;
    return 0;
}

/* Ship one CTERM PDU host -> client and let the CLIENT consume it: a Write
 * lands on the client's screen, byte for byte as the session produced it. */
static int ct_to_term(struct ct_accept *c, const uint8_t *pdu, size_t plen)
{
    uint8_t frame[DNET_FRAME_MAX], rxbuf[DNET_FRAME_MAX], reply[DNET_FRAME_MAX];
    size_t flen = 0, rlen = 0, rxlen = 0;
    int has_reply = 0;
    enum dnet_link_event lev = DNET_LINK_EV_NONE;
    enum dnet_cterm_event cev = DNET_CTERM_EV_NONE;

    if (dnet_engine_link_send(&c->R, pdu, plen, frame, sizeof(frame), &flen, c->t) != 0 ||
        move_frame(c->sv[1], c->sv[0], frame, flen, rxbuf, sizeof(rxbuf), &rxlen) != 0 ||
        dnet_engine_link_rx(&c->L, c->t, rxbuf, rxlen, reply, sizeof(reply), &rlen,
                            &has_reply, &lev) != 0 || lev != DNET_LINK_EV_DATA)
        return -1;
    if (dnet_cterm_rx(&c->term, c->L.rx_data, c->L.rx_datalen, &cev) != DNET_CTERM_OK)
        return -1;
    if (cev == DNET_CTERM_EV_WRITE)
        ct_screen_append(c, c->term.last.data, c->term.last.datalen);
    if (has_reply) {
        if (move_frame(c->sv[0], c->sv[1], reply, rlen, rxbuf, sizeof(rxbuf), &rxlen) != 0 ||
            dnet_engine_link_rx(&c->R, c->t, rxbuf, rxlen, frame, sizeof(frame), &flen,
                                &has_reply, &lev) != 0)
            return -1;
    }
    c->t++;
    return 0;
}

/* Drain whatever the SESSION has written to its terminal and carry it to the
 * client as CTERM Write PDUs, for up to `ms` milliseconds of QUIET or until
 * `expect` (when non-NULL) appears on the client's screen. Returns 1 if
 * `expect` was seen (or expect == NULL), 0 otherwise. */
static int ct_pump(struct ct_accept *c, const char *expect, int ms)
{
    const int step_ms = 20;
    int waited = 0;

    for (;;) {
        uint8_t out[DNET_CTERM_MAX_DATA];
        long n = dnet_cterm_host_read(&c->hs, out, sizeof(out));

        if (n > 0) {
            uint8_t cpdu[DNET_CTERM_MAX_PDU];
            size_t clen = 0;
            if (dnet_cterm_write(&c->hs.cterm, out, (size_t)n,
                                 DNET_CTERM_WR_NOFORMAT, cpdu, sizeof(cpdu), &clen) != 0 ||
                ct_to_term(c, cpdu, clen) != 0)
                return 0;
            waited = 0;             /* progress: give the session more time */
        }
        if (expect && ct_screen_has(c, expect))
            return 1;
        if (n < 0)
            return expect ? 0 : 1;  /* the session's terminal closed */
        if (waited >= ms)
            return expect ? 0 : 1;
        {
            struct timespec ts = { 0, (long)step_ms * 1000000L };
            nanosleep(&ts, NULL);
        }
        waited += step_ms;
    }
}

/* Type a line at the remote LOGINOUT, as the CTERM terminal would. */
static int ct_type(struct ct_accept *c, const char *line)
{
    uint8_t cpdu[DNET_CTERM_MAX_PDU];
    size_t clen = 0;

    if (dnet_cterm_read_data(&c->term, (const uint8_t *)line, strlen(line), 0x0d,
                             cpdu, sizeof(cpdu), &clen) != 0)
        return -1;
    return ct_to_host(c, cpdu, clen);
}

static int g_ct_pass, g_ct_fail;
#define CT_CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", (msg)); g_ct_pass++; } \
    else      { printf("  FAIL: %s\n", (msg)); g_ct_fail++; } \
} while (0)

/* DC$_TERM. Spelled here rather than pulled from dcdef.h so the daemon keeps
 * its existing (deliberately narrow) include set; a divergence shows up as a
 * failing CHECK, not a silently passing one. */
#define CT_DC_TERM  66

static int run_cterm_accept_test(void)
{
    static struct ct_accept c;
    uint8_t frame[DNET_FRAME_MAX], rxbuf[DNET_FRAME_MAX], reply[DNET_FRAME_MAX];
    uint8_t sc[128], cpdu[DNET_CTERM_MAX_PDU];
    size_t flen = 0, rlen = 0, rxlen = 0, sclen = 0, clen = 0;
    int has_reply = 0;
    enum dnet_link_event lev = DNET_LINK_EV_NONE;
    uint32_t st;

    printf("DECNETD-I-CTERMACCEPT, inbound SET HOST -> $CREPRC -> LOGINOUT on RTAn:"
           " (rd vms-f40; oracle docs/oracle/vax-sethost-cterm.*)\n");

    memset(&c, 0, sizeof(c));
    c.hs.master_fd = -1;
    c.t = 100;
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, c.sv) != 0) {
        fprintf(stderr, "DECNETD-E-CTERMACCEPT, socketpair failed: %s\n", strerror(errno));
        return 1;
    }
    {
        const uint8_t hwL[6] = { 0x02,0,0,0,0,0x01 };
        const uint8_t hwR[6] = { 0x02,0,0,0,0,0x02 };
        if (dnet_engine_init(&c.L, 1, 1, "OVMXC", "EWA0", NULL, hwL, 0, 0, 0) != 0 ||
            dnet_engine_init(&c.R, 1, 2, "OVMXH", "EWA0", NULL, hwR, 0, 0, 0) != 0 ||
            dnet_cterm_session_init(&c.term, DNET_CTERM_ROLE_TERMINAL) != 0) {
            fprintf(stderr, "DECNETD-E-CTERMACCEPT, engine init failed\n");
            close(c.sv[0]); close(c.sv[1]);
            return 1;
        }
    }

    /* ---- 1. A REAL inbound connect to object 42, in the oracle's shape ---- */
    if (dnet_cterm_sc_connect_build(DNET_CTERM_OBJECT, "SYSTEM", 0x021a, 0x2020,
                                    "", "", "", sc, sizeof(sc), &sclen) != 0 ||
        dnet_engine_link_open(&c.L, 1, 2, 0x2001, sc, sclen, 1459, 1,
                              DNET_NSP_VER_41, frame, sizeof(frame), &flen, c.t) != 0 ||
        move_frame(c.sv[0], c.sv[1], frame, flen, rxbuf, sizeof(rxbuf), &rxlen) != 0 ||
        dnet_engine_link_rx(&c.R, c.t, rxbuf, rxlen, reply, sizeof(reply), &rlen,
                            &has_reply, &lev) != 0 || lev != DNET_LINK_EV_CONNECT_IND) {
        fprintf(stderr, "DECNETD-E-CTERMACCEPT, the inbound connect never reached the host\n");
        close(c.sv[0]); close(c.sv[1]);
        return 1;
    }
    CT_CHECK(dnet_cterm_sc_connect_object(c.R.link.conn_data, c.R.link.conn_len)
                 == DNET_CTERM_OBJECT,
             "the inbound connect names Session Control OBJECT 42 (CTERM), decoded"
             " off the wire in the oracle's format-0 destination-descriptor shape");

    /* ---- 2. DISPATCH IT, ACROSS THE ISOLATION SEAM (vms-515 §3.4) ---------
     * The wire bytes are parsed at LOW privilege into a validated typed
     * descriptor; ONLY that descriptor is handed to the privileged control
     * path, which mints RTAn: + $CREPRCs LOGINOUT. This is the exact two-step
     * NETACP serve flow -- open-coded here so the test drives the same seam the
     * daemon does, not a convenience wrapper. */
    {
        struct dnet_conn_descriptor desc;
        int prc = dnet_conn_descriptor_from_wire(c.R.link.conn_data,
                                                 c.R.link.conn_len,
                                                 c.R.link.remote_node, &desc);
        CT_CHECK(prc == DNET_CTERM_OK && desc.validated && desc.dst_is_object &&
                     desc.dst_object == DNET_CTERM_OBJECT,
                 "the untrusted connect is parsed at LOW PRIVILEGE into a"
                 " VALIDATED typed descriptor naming object 42 -- the privileged"
                 " path is handed this, never the wire bytes (vms-515 §3.4)");
        st = dnet_cterm_host_open_desc(&c.hs, &desc);
    }
    CT_CHECK((st & 1) != 0,
             "object-42 dispatch created the session through the REAL executive"
             " ($CREPRC PRC$M_INTER|PRC$M_LOGINOUT on an executive-minted RTAn:)");
    if (!(st & 1)) {
        fprintf(stderr, "DECNETD-E-CTERMACCEPT, dnet_cterm_host_open_desc failed, status %08X\n",
                (unsigned)st);
        fprintf(stderr, "  (INV-6: no per-process imitation of a login is substituted;"
                        " a real /dev/vms + SYS$SYSTEM:LOGINOUT.EXE are required)\n");
        goto verdict;
    }
    printf("  INFO: session terminal = %s, session pid = %08X, Remote Port Info = %s\n",
           c.hs.devnam, (unsigned)c.hs.session_pid, c.hs.remote_port_info);

    /* The carried identity is PROXY info and NOTHING ELSE (the oracle's A4/A9
     * answer). It shows up on the accounting surface, and the session is still
     * about to be challenged for a username and a password. The descriptor
     * STRUCTURALLY cannot carry a credential -- it has no password field -- so
     * the "no login on carried identity" property is now enforced by the type,
     * not just measured on this specimen. */
    CT_CHECK(strstr(c.hs.remote_port_info, "::SYSTEM") != NULL,
             "the connect-carried node::user is surfaced as Remote Port Info"
             " (proxy/accounting), exactly as the oracle's SHOW TERMINAL does");

    /* ---- 3. sec-7.5 TELL: $GETDVI the device FROM THIS PROCESS ------------ */
    {
        struct vms_devinfo info;
        uint32_t dst;

        memset(&info, 0, sizeof(info));
        dst = vms_kif_getdvi_devnam(c.hs.devnam, &info);
        CT_CHECK((dst & 1) != 0 && info.devclass == CT_DC_TERM,
                 "$GETDVI on the session's RTAn: from a DIFFERENT process than the"
                 " session returns a real DC$_TERM device row (design sec-7.5 tell:"
                 " a green produced without the executive device table changing is"
                 " a LARP)");
    }

    /* ---- 4. Bind the CTERM session and read LOGINOUT's OWN prompt --------- */
    if (dnet_engine_link_accept(&c.R, 0x2002, reply, sizeof(reply), &rlen, c.t) != 0 ||
        move_frame(c.sv[1], c.sv[0], reply, rlen, rxbuf, sizeof(rxbuf), &rxlen) != 0 ||
        dnet_engine_link_rx(&c.L, c.t, rxbuf, rxlen, frame, sizeof(frame), &flen,
                            &has_reply, &lev) != 0 || lev != DNET_LINK_EV_CONNECT_CONF) {
        fprintf(stderr, "DECNETD-E-CTERMACCEPT, the logical link did not come UP\n");
        g_ct_fail++;
        goto verdict;
    }
    c.t++;
    if (dnet_cterm_bind(&c.term, "OVMXC$RTA1:", cpdu, sizeof(cpdu), &clen) != 0 ||
        ct_to_host(&c, cpdu, clen) != 0 ||
        dnet_cterm_bind_accept(&c.hs.cterm, "OVMXH", cpdu, sizeof(cpdu), &clen) != 0 ||
        ct_to_term(&c, cpdu, clen) != 0 ||
        !dnet_cterm_is_bound(&c.term)) {
        fprintf(stderr, "DECNETD-E-CTERMACCEPT, the CTERM session did not bind\n");
        g_ct_fail++;
        goto verdict;
    }

    /* WAKE THE SESSION. LOGINOUT waits for the operator to strike RETURN
     * before it announces itself and prompts -- gated on being bound to a
     * terminal DEVICE (tools/vms_login.c, loginout_at_operator_terminal()),
     * which this session is, exactly like the console. So the remote terminal
     * types a bare RETURN, as a person at a SET HOST would. This is also,
     * incidentally, a second proof that the executive recorded the terminal
     * binding: if it had not, LOGINOUT would not be waiting for a RETURN.
     *
     * ONE RETURN, THEN A LONG WAIT -- not a fast retry loop. The terminal
     * buffers input, so a RETURN typed before the session process reaches its
     * read is still there when it does (and LOGINOUT's own type-ahead flush
     * runs AFTER that read, so it cannot eat the wake). A machine-gun of
     * RETURNs, by contrast, can land one in the window between the flush and
     * the prompt, where it reads as an EMPTY USERNAME -- burning one of
     * LOGINOUT's three attempts and leaving too few for the three refusals
     * this test needs. One keystroke, patiently, is both more faithful and
     * more robust. A single retry after a long silence cannot race a prompt
     * that would already have been seen.
     */
    {
        int woke = 0, tries;
        for (tries = 0; tries < 2 && !woke; tries++) {
            if (ct_type(&c, "") != 0)
                break;
            woke = ct_pump(&c, "Username:", tries == 0 ? 20000 : 10000);
        }
        CT_CHECK(woke,
                 "the inbound SET HOST is CHALLENGED: LOGINOUT's own Username:"
                 " prompt arrives over the link (a no-auth CTERM would answer"
                 " with a bare $)");
        if (!woke)
            goto verdict;
    }

    /* ---- 5. REJECTION IS THE PROOF: bad credentials are refused ----------- */
    if (ct_type(&c, "NOSUCHUSER") == 0 && ct_pump(&c, "Password:", 15000)) {
        (void)ct_type(&c, "WRONGPASSWORD");
        CT_CHECK(ct_pump(&c, "authorization failure", 20000),
                 "a nonexistent account is REJECTED by LOGINOUT over the CTERM link"
                 " (%LOGIN-F-INVPWD, user authorization failure)");
    } else {
        CT_CHECK(0, "LOGINOUT solicited a password for the offered username");
    }

    /* A REAL account with a WRONG password must be refused too -- otherwise the
     * refusal above could be unknown-user handling rather than authentication. */
    if (ct_pump(&c, "Username:", 20000) && ct_type(&c, "SYSTEM") == 0 &&
        ct_pump(&c, "Password:", 15000)) {
        (void)ct_type(&c, "NOTTHEPASSWORD");
        CT_CHECK(ct_pump(&c, "authorization failure", 20000),
                 "a REAL account with a WRONG password is REJECTED (so the refusal"
                 " above is authentication, not unknown-user handling)");
    } else {
        CT_CHECK(0, "LOGINOUT re-prompted after the first authorization failure");
    }

    /* DISUSER, THE THIRD AND SHARPEST REFUSAL. DISABLED's password is CORRECT
     * (tools/mksysuaf.c seeds it deliberately valid), so the only thing that
     * can refuse this login is the SYSUAF login-flag rule -- "a correct
     * password is not sufficient" (vms-c8fa). A CTERM path that bypassed
     * LOGINOUT, or reached a LOGINOUT that skipped the flag check for network
     * logins, admits this account and fails here. This is LOGINOUT's third
     * attempt, so it is also the last one before it drops the connection
     * (MAX_ATTEMPTS = 3, tools/vms_login.c) -- which is why it goes last. */
    if (ct_pump(&c, "Username:", 20000) && ct_type(&c, "DISABLED") == 0 &&
        ct_pump(&c, "Password:", 15000)) {
        (void)ct_type(&c, "DISABLED");
        CT_CHECK(ct_pump(&c, "authorization failure", 20000),
                 "DISUSER IS HONOURED over CTERM: the DISABLED account is refused"
                 " even though the password typed was CORRECT -- the refusal can"
                 " only be the SYSUAF login-flag rule");
        CT_CHECK(!ct_screen_has(&c, "Welcome to OpenVMX"),
                 "...and it never reached a session banner");
    } else {
        CT_CHECK(0, "LOGINOUT re-prompted after the second authorization failure");
    }

    /* NO SESSION WAS EVER ADMITTED. The whole run typed three credential sets,
     * every one of which had to be refused; if any DCL prompt or welcome banner
     * appeared on the client's screen, an unauthenticated (or wrongly
     * authenticated) session was handed to the peer -- which is the exact
     * defect this item exists to close. */
    CT_CHECK(!ct_screen_has(&c, "Welcome to OpenVMX") &&
             !ct_screen_has(&c, "\n$ ") && !ct_screen_has(&c, "\r$ "),
             "NO session was admitted anywhere in this run: no welcome banner and"
             " no DCL prompt ever reached the remote terminal");

    /* NEGCTL: prove the screen search is not vacuous -- it must FIND a token
     * the session really produced and REJECT one it never could. Without this,
     * every "was CHALLENGED" PASS above could be a search over an empty
     * buffer that happened to be scored the right way. */
    CT_CHECK(ct_screen_has(&c, "Username:") &&
             !ct_screen_has(&c, "ZZ_NOT_ON_THIS_SCREEN_ZZ"),
             "NEGCTL: the screen search finds a token the session really sent and"
             " rejects one it never sent -- the assertions above can go red");

    /* ---- 6. Tear down and prove the device row went with the session ------ */
    {
        char devnam[DNET_CTERM_HOST_DEVNAM];
        struct vms_devinfo info;

        snprintf(devnam, sizeof(devnam), "%s", c.hs.devnam);
        (void)dnet_cterm_host_close(&c.hs);
        memset(&info, 0, sizeof(info));
        CT_CHECK((vms_kif_getdvi_devnam(devnam, &info) & 1) == 0,
                 "the RTAn: row is WITHDRAWN from the executive when the session"
                 " ends -- it appeared with the session and disappears with it");
    }

verdict:
    if (c.hs.master_fd >= 0)
        (void)dnet_cterm_host_close(&c.hs);
    close(c.sv[0]);
    close(c.sv[1]);
    printf("DECNETD-I-CTERMACCEPT, %d passed, %d failed\n", g_ct_pass, g_ct_fail);
    if (g_ct_fail == 0 && g_ct_pass > 0) {
        printf("DECNETD-CTERM-ACCEPT: PASS\n");
        return 0;
    }
    printf("DECNETD-CTERM-ACCEPT: FAIL\n");
    return 1;
}

/*
 * ===================== --isolation-test (rd vms-9ab) =====================
 * THE A2/A8 ISOLATION PROOF, privileged half. --cterm-accept-test (above)
 * proves the POSITIVE path end to end on a booted image; this proves the
 * NEGATIVE contract of the seam, and it needs NEITHER CAP_NET_RAW NOR
 * /dev/vms, because every case here is REFUSED at NETACP's privileged front
 * door BEFORE it would touch the executive:
 *
 *   - dnet_cterm_host_open_desc() -- the privileged control path that mints
 *     RTAn: and $CREPRCs LOGINOUT -- takes ONLY a validated typed descriptor.
 *     Handed an UNVALIDATED descriptor (the state a malformed frame leaves) or
 *     one naming any object but 42, it returns SS$_BADPARAM and creates NO
 *     device and NO process (master_fd stays -1). So a fuzzed inbound frame,
 *     whose low-privilege parse fails, cannot reach the session-creating code.
 *
 *   - the double-door: for a mutation-fuzz corpus, EVERY frame the low-priv
 *     parse (dnet_conn_descriptor_from_wire) rejects is ALSO refused by the
 *     privileged path -- the two doors agree, and neither opens on hostile
 *     bytes. This is run here (not only in the pure unit test) so the SAME
 *     binary that serves the wire is the one proven to hold the door.
 *
 * Returns 0 on PASS, 1 on FAIL.
 */
static int run_isolation_test(void)
{
    struct dnet_cterm_host_session hs;
    struct dnet_conn_descriptor d;
    uint32_t st;
    int pass = 0, fail = 0;

    printf("DECNETD-I-ISOLATION, the A2/A8 privileged-path isolation proof"
           " (rd vms-9ab; design vms-515 §3.4; runs off-target, needs neither"
           " CAP_NET_RAW nor a booted executive)\n");

    /* 1. An UNVALIDATED (all-zero) descriptor is refused, nothing created. */
    memset(&hs, 0, sizeof(hs)); hs.master_fd = -1;
    memset(&d, 0, sizeof(d));   /* validated == 0 */
    st = dnet_cterm_host_open_desc(&hs, &d);
    if (!(st & 1) && hs.master_fd == -1 && hs.active == 0) {
        printf("  PASS: an UNVALIDATED descriptor is refused (%08X); no device,"
               " no process (the state a malformed frame leaves)\n", (unsigned)st);
        pass++;
    } else { printf("  FAIL: an unvalidated descriptor was not cleanly refused\n"); fail++; }

    /* 2. A VALIDATED descriptor naming the WRONG object (17 = FAL, not built)
     *    is refused -- INV-6: a known-but-unbuilt object is not faked. */
    memset(&hs, 0, sizeof(hs)); hs.master_fd = -1;
    memset(&d, 0, sizeof(d));
    d.validated = 1; d.dst_is_object = 1; d.dst_object = DNET_OBJ_FAL;
    st = dnet_cterm_host_open_desc(&hs, &d);
    if (!(st & 1) && hs.master_fd == -1 && hs.active == 0) {
        printf("  PASS: a validated descriptor for object 17 (FAL, unbuilt) is"
               " refused (%08X); no fabricated session (INV-6)\n", (unsigned)st);
        pass++;
    } else { printf("  FAIL: a wrong-object descriptor was not cleanly refused\n"); fail++; }

    /* 3. A validated NAMED-TASK descriptor (not a well-known object) is refused
     *    by this CTERM dispatch. */
    memset(&hs, 0, sizeof(hs)); hs.master_fd = -1;
    memset(&d, 0, sizeof(d));
    d.validated = 1; d.dst_is_object = 0; d.dst_object = DNET_CTERM_OBJECT;
    st = dnet_cterm_host_open_desc(&hs, &d);
    if (!(st & 1) && hs.master_fd == -1) {
        printf("  PASS: a named-task descriptor (not a well-known object) is"
               " refused (%08X)\n", (unsigned)st);
        pass++;
    } else { printf("  FAIL: a named-task descriptor was not cleanly refused\n"); fail++; }

    /* 4. THE DOUBLE-DOOR under mutation fuzz: every frame the low-priv parse
     *    rejects, the privileged path also refuses -- proven on THIS binary. */
    {
        unsigned seed = 0x9abd0000u & 0x7fffffff, i, mism = 0, reached = 0;
        for (i = 0; i < 100000; i++) {
            uint8_t mbuf[64];
            size_t mlen = (size_t)(rand_r(&seed) % sizeof(mbuf)), j;
            int muts, m;
            static const uint8_t seedmsg[10] =
                { 0x00, 0x2a, 0x02, 0x00, 0x00, 0x00, 0x21, 0x84, 0x02, 0x27 };
            for (j = 0; j < mlen; j++)
                mbuf[j] = j < sizeof(seedmsg) ? seedmsg[j]
                                              : (uint8_t)(rand_r(&seed) & 0xff);
            muts = 1 + (rand_r(&seed) % 3);
            for (m = 0; m < muts && mlen; m++)
                mbuf[rand_r(&seed) % mlen] = (uint8_t)(rand_r(&seed) & 0xff);

            int rc = dnet_conn_descriptor_from_wire(mbuf, mlen, 1025, &d);
            if (rc != DNET_CTERM_OK) {
                reached++;
                memset(&hs, 0, sizeof(hs)); hs.master_fd = -1;
                st = dnet_cterm_host_open_desc(&hs, &d);
                if ((st & 1) || hs.master_fd != -1 || hs.active)
                    mism++;
            }
        }
        if (mism == 0 && reached > 10000) {
            printf("  PASS: double-door fuzz -- %u parse-rejected frames, EVERY"
                   " one also refused by the privileged path (no device/process)\n",
                   reached);
            pass++;
        } else {
            printf("  FAIL: double-door fuzz mism=%u reached=%u\n", mism, reached);
            fail++;
        }
    }

    printf("DECNETD-I-ISOLATION, %d passed, %d failed\n", pass, fail);
    if (fail == 0 && pass > 0) { printf("DECNETD-ISOLATION: PASS\n"); return 0; }
    printf("DECNETD-ISOLATION: FAIL\n");
    return 1;
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
        "  --cterm-accept-test run the INBOUND SET HOST acceptance and exit: a\n"
        "                      real connect to Session Control object 42 is\n"
        "                      dispatched through the executive ($CREPRC\n"
        "                      PRC$M_INTER|PRC$M_LOGINOUT on an executive-minted\n"
        "                      RTAn:) and the REAL LOGINOUT.EXE must CHALLENGE it\n"
        "                      and REFUSE bad credentials. Needs /dev/vms +\n"
        "                      SYS$SYSTEM:LOGINOUT.EXE; no CAP_NET_RAW.\n"
        "  --isolation-test    run the A2/A8 privileged-path isolation proof and\n"
        "                      exit (needs neither CAP_NET_RAW nor an executive):\n"
        "                      an unvalidated\n"
        "                      or wrong-object descriptor is refused by NETACP's\n"
        "                      privileged control path before any device/process\n"
        "                      exists, and a mutation-fuzz corpus the low-priv\n"
        "                      parse rejects is refused there too (double-door).\n"
        "  --cterm-server      serve inbound $ SET HOST on the live datalink:\n"
        "                      accept a logical link to object 42 and create a\n"
        "                      process running LOGINOUT.EXE on an RTAn: for it.\n"
        "                      The remote user is AUTHENTICATED by LOGINOUT --\n"
        "                      this daemon spawns nothing and knows no password.\n",
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
    int cterm_accept_test = 0;
    int isolation_test = 0;
    int cterm_server = 0;

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
        else if (!strcmp(argv[i], "--cterm-accept-test")) cterm_accept_test = 1;
        else if (!strcmp(argv[i], "--isolation-test")) isolation_test = 1;
        else if (!strcmp(argv[i], "--cterm-server")) cterm_server = 1;
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
    if (cterm_accept_test)
        return run_cterm_accept_test();
    if (isolation_test)
        return run_isolation_test();

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

    /* Startup: the VMS-visible face (never the raw socket). NETACP model
     * (vms-9ab, P5): this process is DECnet's session-control ACP; the wire
     * engine below is its low-privilege DATALINK, and the AF_PACKET socket is
     * hidden behind the executive device face _NET: (Rule 1, vms-515 §3.3). */
    log_ts(stdout);
    printf(" DECNETD-I-STARTED, NETACP up: DECnet Phase IV endnode on circuit %s"
           " (wire engine demoted to NETACP's datalink; AF_PACKET hidden behind"
           " the _NET: device face, Rule 1)\n", eng.circuit);
    dnet_engine_show_executor(&eng, stdout);
    dnet_engine_show_circuit(&eng, stdout);
    fflush(stdout);

    uint8_t frame[DNET_FRAME_MAX];
    uint8_t rxbuf[DNET_FRAME_MAX];

    /* The inbound-$-SET-HOST session this endnode is currently serving
     * (--cterm-server). One at a time in this rung; the state is only ever the
     * executive's device name plus the CTERM FSM -- no credential, no shell. */
    struct dnet_cterm_host_session host;
    int host_active = 0;
    char session_devnam[DNET_CTERM_HOST_DEVNAM] = {0};
    uint8_t peer_mac[6] = {0};

    memset(&host, 0, sizeof(host));
    host.master_fd = -1;

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

        /*
         * SERVE THE LIVE SESSION'S TERMINAL (rd vms-f40, --cterm-server).
         *
         * Whatever LOGINOUT/DCL has written to the session's RTAn: is carried
         * to the remote terminal as CTERM Write PDUs inside NSP data segments.
         * This daemon is a PIPE here and nothing more: it holds no credential,
         * makes no login decision and spawns nothing -- the process on the
         * other end of that terminal is the one authenticating, and it was
         * created by $CREPRC, not by this program.
         *
         * poll() rather than the bare 1-second datalink timeout, so an
         * interactive session is not typed into at one character a second;
         * the 250 ms cap still lets the T3 cadence and the listen sweep fire.
         */
        if (cterm_server && host_active) {
            struct pollfd pfd[2];
            int nfds = 1;

            pfd[0].fd = sock;         pfd[0].events = POLLIN; pfd[0].revents = 0;
            pfd[1].fd = dnet_cterm_host_fd(&host); pfd[1].events = POLLIN; pfd[1].revents = 0;
            if (pfd[1].fd >= 0)
                nfds = 2;
            (void)poll(pfd, (unsigned)nfds, 250);

            if (nfds == 2 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
                uint8_t out[DNET_CTERM_MAX_DATA];
                long got = dnet_cterm_host_read(&host, out, sizeof(out));

                if (got > 0 && dnet_cterm_is_bound(&host.cterm)) {
                    uint8_t cpdu[DNET_CTERM_MAX_PDU], dframe[DNET_FRAME_MAX];
                    size_t clen = 0, dlen = 0;
                    if (dnet_cterm_write(&host.cterm, out, (size_t)got,
                                         DNET_CTERM_WR_NOFORMAT, cpdu,
                                         sizeof(cpdu), &clen) == 0 &&
                        dnet_engine_link_send(&eng, cpdu, clen, dframe,
                                              sizeof(dframe), &dlen, now) == 0)
                        (void)scs_datalink_send(sock, (int)ifindex, DNET_ETHERTYPE,
                                                peer_mac, dframe, dlen);
                } else if (got < 0 || !dnet_cterm_host_alive(&host)) {
                    /* The session ended (it logged out, or LOGINOUT refused and
                     * exited). Release the terminal and tear the link down --
                     * the same order the oracle's LOGOUT produced.
                     *
                     * TWO INDEPENDENT WAYS TO NOTICE, and the executive is the
                     * authoritative one. `got < 0` is the substrate telling us
                     * the terminal channel closed; dnet_cterm_host_alive() ASKS
                     * THE EXECUTIVE whether the session process still has a row
                     * ($GETJPI on the pid $CREPRC returned). An interactive
                     * process is ownerless -- the top of its own job -- so this
                     * daemon has no child to waitpid() for and could not learn
                     * it any other way; the same reason JOB_CONTROL reads the
                     * console session's life out of the executive. */
                    size_t dlen = 0;
                    if (dnet_cterm_unbind(&host.cterm, DNET_CTERM_UNBIND_NORMAL,
                                          frame, sizeof(frame), &dlen) == 0) {
                        uint8_t dframe[DNET_FRAME_MAX];
                        size_t flen2 = 0;
                        if (dnet_engine_link_send(&eng, frame, dlen, dframe,
                                                  sizeof(dframe), &flen2, now) == 0)
                            (void)scs_datalink_send(sock, (int)ifindex,
                                                    DNET_ETHERTYPE, peer_mac,
                                                    dframe, flen2);
                    }
                    {
                        size_t flen2 = 0;
                        if (dnet_engine_link_close(&eng, DNET_LINK_REASON_NORMAL,
                                                   frame, sizeof(frame), &flen2,
                                                   now) == 0)
                            (void)scs_datalink_send(sock, (int)ifindex,
                                                    DNET_ETHERTYPE, peer_mac,
                                                    frame, flen2);
                    }
                    (void)dnet_cterm_host_close(&host);
                    host_active = 0;
                    log_ts(stdout);
                    printf(" DECNETD-I-SESSEND, inbound SET HOST session on %s ended\n",
                           session_devnam);
                    fflush(stdout);
                }
            }
            if (!(pfd[0].revents & POLLIN))
                continue;             /* nothing on the wire this round */
        }

        ssize_t n = scs_datalink_recv(sock, rxbuf, sizeof(rxbuf));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue; /* timer wakeup / signal -- re-check cadence */
            fprintf(stderr, "DECNETD-E-RECVFAIL, recv failed: %s\n", strerror(errno));
            break;
        }
        char afrom[8];
        uint8_t from[6];
        enum dnet_adj_state st = DNET_ADJ_DOWN;
        int rc = dnet_engine_rx_frame(&eng, now, rxbuf, (size_t)n, from, &st);

        /*
         * INBOUND $ SET HOST DISPATCH (rd vms-f40, --cterm-server). A frame the
         * routing/HELLO path did not claim may be an NSP logical-link frame.
         * On a CONNECT INDICATION we decode the Session Control connect --
         * UNTRUSTED, UNAUTHENTICATED bytes, parsed under bounds -- and, if it
         * names object 42, hand it to dnet_cterm_host_open(), which mints an
         * RTAn: in the executive and $CREPRCs LOGINOUT.EXE onto it.
         *
         * WHAT THIS DAEMON DOES NOT DO, and must never do again: it does not
         * openpty, does not fork, does not exec, and does not decide that
         * anyone may log in. The connect-carried username (the oracle's
         * "Remote Port Info") is accounting information and reaches no
         * decision. Every refusal below leaves the peer disconnected rather
         * than admitted (INV-6).
         */
        if (cterm_server && rc != 1) {
            uint8_t reply[DNET_FRAME_MAX];
            size_t rlen = 0;
            int has_reply = 0;
            enum dnet_link_event lev = DNET_LINK_EV_NONE;

            if (dnet_engine_link_rx(&eng, now, rxbuf, (size_t)n, reply,
                                    sizeof(reply), &rlen, &has_reply, &lev) == 0) {
                if (has_reply)
                    (void)scs_datalink_send(sock, (int)ifindex, DNET_ETHERTYPE,
                                            rxbuf + 6, reply, rlen);

                if (lev == DNET_LINK_EV_CONNECT_IND) {
                    int obj = dnet_cterm_sc_connect_object(eng.link.conn_data,
                                                           eng.link.conn_len);
                    uint32_t cst;
                    size_t flen2 = 0;

                    memcpy(peer_mac, rxbuf + 6, 6);
                    if (host_active || obj != DNET_CTERM_OBJECT) {
                        /* One session at a time in this rung, and only object
                         * 42 is served here. Refuse on the wire; admit nobody. */
                        if (dnet_engine_link_close(&eng, DNET_LINK_REASON_OBJREJ,
                                                   frame, sizeof(frame), &flen2,
                                                   now) == 0)
                            (void)scs_datalink_send(sock, (int)ifindex,
                                                    DNET_ETHERTYPE, peer_mac,
                                                    frame, flen2);
                        log_ts(stdout);
                        printf(" DECNETD-I-CONNREJ, inbound connect to object %d"
                               " refused (%s)\n", obj,
                               host_active ? "a session is already active"
                                           : "no such object served here");
                        fflush(stdout);
                    } else {
                        /* THE ISOLATION SEAM (vms-515 §3.4). Parse the untrusted
                         * connect at low privilege into a validated typed
                         * descriptor, then hand ONLY that to the privileged
                         * control path. The privileged path never sees
                         * eng.link.conn_data. A malformed frame fails the parse
                         * here and the peer is refused below like any other
                         * unservable connect. */
                        struct dnet_conn_descriptor desc;
                        if (dnet_conn_descriptor_from_wire(eng.link.conn_data,
                                                           eng.link.conn_len,
                                                           eng.link.remote_node,
                                                           &desc) != DNET_CTERM_OK)
                            cst = SS$_BADPARAM;
                        else
                            cst = dnet_cterm_host_open_desc(&host, &desc);
                        if (!(cst & 1)) {
                            /* No session, no shell, no fallback. */
                            if (dnet_engine_link_close(&eng, DNET_LINK_REASON_OBJREJ,
                                                       frame, sizeof(frame),
                                                       &flen2, now) == 0)
                                (void)scs_datalink_send(sock, (int)ifindex,
                                                        DNET_ETHERTYPE, peer_mac,
                                                        frame, flen2);
                            fprintf(stderr, "DECNETD-E-NOSESSION, inbound SET HOST"
                                    " refused: the session could not be created"
                                    " (status %08X); no unauthenticated shell is"
                                    " substituted\n", (unsigned)cst);
                        } else {
                            host_active = 1;
                            snprintf(session_devnam, sizeof(session_devnam), "%s",
                                     host.devnam);
                            if (dnet_engine_link_accept(&eng, 0x2002, frame,
                                                        sizeof(frame), &flen2,
                                                        now) == 0)
                                (void)scs_datalink_send(sock, (int)ifindex,
                                                        DNET_ETHERTYPE, peer_mac,
                                                        frame, flen2);
                            log_ts(stdout);
                            printf(" DECNETD-I-SESSTART, inbound SET HOST accepted"
                                   " on %s -- LOGINOUT is authenticating"
                                   " (Remote Port Info: %s)\n",
                                   host.devnam, host.remote_port_info);
                            fflush(stdout);
                        }
                    }
                } else if (lev == DNET_LINK_EV_DATA && host_active) {
                    /* Terminal bytes from the remote. The CTERM FSM decodes
                     * them; a Bind is answered, keystrokes go to the session's
                     * terminal -- to LOGINOUT, which is what decides whether
                     * they are a valid login. */
                    enum dnet_cterm_event cev = DNET_CTERM_EV_NONE;

                    if (dnet_cterm_rx(&host.cterm, eng.rx_data, eng.rx_datalen,
                                      &cev) == DNET_CTERM_OK) {
                        uint8_t cpdu[DNET_CTERM_MAX_PDU], dframe[DNET_FRAME_MAX];
                        size_t clen = 0, dlen = 0;

                        if (cev == DNET_CTERM_EV_BIND_IND &&
                            dnet_cterm_bind_accept(&host.cterm, eng.node_name, cpdu,
                                                   sizeof(cpdu), &clen) == 0 &&
                            dnet_engine_link_send(&eng, cpdu, clen, dframe,
                                                  sizeof(dframe), &dlen, now) == 0)
                            (void)scs_datalink_send(sock, (int)ifindex,
                                                    DNET_ETHERTYPE, peer_mac,
                                                    dframe, dlen);
                        else if (cev == DNET_CTERM_EV_READ_DATA) {
                            if (host.cterm.last.datalen)
                                (void)dnet_cterm_host_write(&host,
                                        host.cterm.last.data,
                                        host.cterm.last.datalen);
                            if (host.cterm.last.terminator) {
                                uint8_t nl = host.cterm.last.terminator;
                                (void)dnet_cterm_host_write(&host, &nl, 1);
                            }
                        } else if (cev == DNET_CTERM_EV_OOB) {
                            uint8_t ob = host.cterm.last.oob_char;
                            (void)dnet_cterm_host_write(&host, &ob, 1);
                        } else if (cev == DNET_CTERM_EV_UNBOUND) {
                            (void)dnet_cterm_host_close(&host);
                            host_active = 0;
                        }
                    }
                } else if (lev == DNET_LINK_EV_DISCONNECT && host_active) {
                    (void)dnet_cterm_host_close(&host);
                    host_active = 0;
                    log_ts(stdout);
                    printf(" DECNETD-I-SESSEND, the remote disconnected the"
                           " SET HOST session on %s\n", session_devnam);
                    fflush(stdout);
                }
            }
        }

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
           " frames_dropped=%lu adj_up=%lu adj_down=%lu\n",
           eng.hello_sent, eng.hello_recv, eng.frames_recv, eng.frames_dropped,
           eng.adj_up_events, eng.adj_down_events);
    fflush(stdout);

    scs_datalink_close(sock);
    return 0;
}
