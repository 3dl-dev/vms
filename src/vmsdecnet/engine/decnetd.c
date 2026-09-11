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
#include <pthread.h>    /* --fal-accept-test / --fal-selftest: two blocking peers */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    /* strcasecmp for --set-host node-name resolution */
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "dnet_engine.h"
#include "dnet_cterm.h"     /* CTERM terminal-service protocol (--set-host-selftest) */
#include "dnet_cterm_host.h" /* CTERM HOST session: $CREPRC -> LOGINOUT on RTAn: */
#include "dnet_dap.h"       /* DAP message codec (--fal-* : COPY presentation layer) */
#include "dnet_fal.h"       /* FAL server + COPY client (object 17, rd vms-8c2) */
#include "rms_textfile.h"   /* --fal-accept-test byte-verify: RMS over the ACP */
#include "ovmx_identity.h"  /* INV-1 identity SSOT: human banner = OVMX product id */
#include "scs_datalink.h"   /* the shared raw-L2 datalink (src/libdatalink) */
#include "ssdef.h"          /* SS$_BADPARAM (isolation-seam refusal, vms-9ab) */
#include "vms_kif.h"        /* $GETDVI readback: the sec-7.5 anti-LARP tell */
#include "starlet.h"        /* vms-f54 CLIENT: $ASSIGN/$QIO(W)/$DASSGN terminal I/O */
#include "descrip.h"        /* dsc$descriptor_s for the SYS$INPUT/SYS$OUTPUT assign */
#include "iodef.h"          /* IO$_READVBLK/WRITEVBLK/SETMODE + IO$K_TT_PASSALL      */

/* The executive terminal channel's backing fd, so the client can poll() the
 * datalink AND the terminal for readiness in one wait -- readiness only; every
 * byte still MOVES through $QIO on the assigned channel (vms-f54, vms-1c57). */
extern int vms$$chan_to_fd(uint16_t chan);

/* The process context the system services need for their channel table. An
 * OVMX image activated by the executive already holds one; a DECNETD.EXE run
 * standalone (the veth test harness) does not, so the --set-host client
 * establishes one itself before it $ASSIGNs its terminal -- the same bootstrap
 * vmssshd and DCL do (src/vmsssh/vmssshd.c, src/vmsdcl/dcl_main.c). */
struct vms_pcb;
extern struct vms_pcb *vms_pcb_get(void);
extern struct vms_pcb *vms_pcb_init(uint64_t initial_privs);

/* The live --set-host CI's nonzero format-2 source group/user codes, sourced
 * from the running process (vms-15a/a70). Defined with run_set_host_loop; the
 * --set-host-src-codes-selftest above it asserts the codes are nonzero. */
static void sethost_src_codes(uint16_t *grp, uint16_t *usr);

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
 * DECnet configuration self-sourcing for the --set-host CLIENT (rd vms-f54).
 *
 * DCL's SET HOST wiring activates "DECNETD.EXE --set-host <node> [--user ...]"
 * on the caller's terminal and leaves the DECnet configuration to the daemon --
 * correct layering: DCL knows nothing of DECnet internals. So in client mode the
 * daemon reads its OWN executor address and resolves a target NODE NAME from the
 * node's DECnet configuration files, the SAME files NCP writes (src/vmsdecnet/
 * ncp/): the executor database and the node database. Paths match ncp.c exactly
 * (env override, then the /etc/ovmx/decnet defaults) so there is ONE config SSOT,
 * never a second ledger. Missing/uncofigured -> honest failure, never a guess.
 */
static const char *decnet_executor_path(void)
{
    const char *p = getenv("OVMX_DECNET_EXECUTOR");
    return (p && p[0]) ? p : "/etc/ovmx/decnet/executor.dat";
}
static const char *decnet_nodedb_path(void)
{
    const char *p = getenv("OVMX_DECNET_NODEDB");
    return (p && p[0]) ? p : "/etc/ovmx/decnet/netnode_remote.dat";
}

/* Read the local executor address from executor.dat ("EXECUTOR <a.n> NAME <name>
 * STATE <on|off>", the ncp.c format). Returns 0 and fills area/node on success. */
static int sethost_source_executor(unsigned *area, unsigned *node)
{
    FILE *f = fopen(decnet_executor_path(), "r");
    if (!f)
        return -1;
    char astr[32] = "", name[64] = "", st[16] = "";
    int ok = -1;
    if (fscanf(f, "EXECUTOR %31s NAME %63s STATE %15s", astr, name, st) == 3 &&
        parse_addr(astr, area, node) == 0)
        ok = 0;
    fclose(f);
    return ok;
}

/* Resolve a --set-host target: accept "area.node" directly, else look the token
 * up as a NODE NAME (case-insensitive) in netnode_remote.dat ("NODE <a.n> [NAME
 * <name>]", the dnet_nodedb_save format). Returns 0 and fills area/node. */
static int sethost_resolve_target(const char *token, unsigned *area, unsigned *node)
{
    if (parse_addr(token, area, node) == 0)
        return 0;
    FILE *f = fopen(decnet_nodedb_path(), "r");
    if (!f)
        return -1;
    char line[256];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;
        char kw[16], astr[32], namekw[16], nm[64];
        int nf = sscanf(p, "%15s %31s %15s %63s", kw, astr, namekw, nm);
        if (nf >= 4 && strcmp(kw, "NODE") == 0 && strcmp(namekw, "NAME") == 0 &&
            strcasecmp(nm, token) == 0 && parse_addr(astr, area, node) == 0) {
            found = 0;
            break;
        }
    }
    fclose(f);
    return found;
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
 * ======================= --router --self-test ========================
 * The router run-mode analogue of run_self_test (rd vms-0a9): a no-privilege,
 * no-netdev, NO-REAL-NODE proof of the router-hello EMIT path. It stands up a
 * ROUTER (R, --router) and an ENDNODE (E), and over a real socketpair(2):
 *
 *   1. R builds its spec-faithful router-hello frame (the exact bytes the live
 *      datalink would put on AB-00-00-04-00-00) and ships them to E.
 *   2. E consumes them through dnet_engine_rx_frame -- the same routing path the
 *      live wire drives -- and SELECTS R as its designated router (E.have_dr,
 *      dr_id == R's id): the endnode picked the router.
 *   3. E now emits an endnode-hello that NAMES R in its rtr/neighbor field (the
 *      passive-capture signal vms-aac0 will look for on real VAX wire), and we
 *      ship it back to R.
 *   4. R (a router) consumes E's endnode-hello and, because E now names R, the
 *      two-way adjacency to E reaches UP -- the loop closes with no crash.
 *
 * This proves emit -> decode -> DR-selection -> reflected-neighbour end to end
 * over genuine write(2)/read(2) of the actual encoded bytes, entirely between
 * two OVMX engines. It touches NO real node (⭐⭐ never-crash-a-peer: the router-
 * hello is proven safe HERE, in isolation, before any live emission).
 *
 * Returns 0 on PASS, 1 on FAIL.
 */
static int run_router_self_test(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        fprintf(stderr, "DECNETD-E-ROUTERTEST, socketpair failed: %s\n",
                strerror(errno));
        return 1;
    }

    const uint8_t hwR[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x11 };
    const uint8_t hwE[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x12 };
    struct dnet_engine R, E;
    /* router = 1.42 (OVMXR), endnode = 1.11 (OVMXE). */
    if (dnet_engine_init(&R, 1, 42, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0) != 0 ||
        dnet_engine_init(&E, 1, 11, "OVMXE", "EWA0", NULL, hwE, 0, 0, 0) != 0) {
        fprintf(stderr, "DECNETD-E-ROUTERTEST, engine init failed\n");
        close(sv[0]); close(sv[1]);
        return 1;
    }
    if (dnet_engine_set_router(&R, 64) != 0 || !dnet_engine_is_router(&R)) {
        fprintf(stderr, "DECNETD-E-ROUTERTEST, set_router failed\n");
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

    /* 1) ROUTER emits its router-hello -> ENDNODE. The emitted node-type bits
     *    must say "router" so an endnode treats it as a DR candidate. */
    if (dnet_engine_build_router_hello_frame(&R, frame, sizeof(frame), &flen) != 0) {
        fprintf(stderr, "DECNETD-E-ROUTERTEST, build router-hello failed\n");
        fail = 1; goto done;
    }
    /* Assert node-type bits on the wire == L1 router. IINFO is at payload
     * offset 12 (the router-hello map, dnet_router_hello.h), payload starting at
     * frame + ETH_HDRLEN; the low 2 bits carry the node type. */
    if ((frame[DNET_ETH_HDRLEN + 12] & 0x03u) != DNET_NODETYPE_L1ROUTER) {
        fprintf(stderr, "DECNETD-E-ROUTERTEST, emitted node-type is not router\n");
        fail = 1; goto done;
    }
    if (write(sv[0], frame, flen) != (ssize_t)flen) {
        fprintf(stderr, "DECNETD-E-ROUTERTEST, write failed: %s\n", strerror(errno));
        fail = 1; goto done;
    }
    n = read(sv[1], rxbuf, sizeof(rxbuf));
    if (n <= 0) {
        fprintf(stderr, "DECNETD-E-ROUTERTEST, read failed: %s\n", strerror(errno));
        fail = 1; goto done;
    }
    /* 2) ENDNODE consumes it through the routing path and SELECTS R as its DR. */
    if (dnet_engine_rx_frame(&E, now, rxbuf, (size_t)n, from, &st) != 1) {
        fprintf(stderr, "DECNETD-E-ROUTERTEST, E did not accept the router-hello\n");
        fail = 1; goto done;
    }
    if (!E.have_dr || memcmp(E.dr_id, R.my_id, 6) != 0) {
        fprintf(stderr, "DECNETD-E-ROUTERTEST, E did not select R as its DR\n");
        fail = 1; goto done;
    }

    /* 3) ENDNODE now emits an endnode-hello that NAMES R in its rtr field, and
     *    4) ROUTER consumes it -> two-way adjacency to E reaches UP. */
    if (dnet_engine_build_hello_frame(&E, frame, sizeof(frame), &flen) != 0) {
        fprintf(stderr, "DECNETD-E-ROUTERTEST, build E endnode-hello failed\n");
        fail = 1; goto done;
    }
    /* The rtr/neighbor field is at endnode-hello payload offset 24 (dnet_hello.h
     * map), payload starting at frame + ETH_HDRLEN; it must now name R. */
    if (memcmp(frame + DNET_ETH_HDRLEN + 24, R.my_id, 6) != 0) {
        fprintf(stderr, "DECNETD-E-ROUTERTEST, E's endnode-hello does not name R\n");
        fail = 1; goto done;
    }
    if (write(sv[1], frame, flen) != (ssize_t)flen) {
        fprintf(stderr, "DECNETD-E-ROUTERTEST, write2 failed\n");
        fail = 1; goto done;
    }
    n = read(sv[0], rxbuf, sizeof(rxbuf));
    if (n <= 0) {
        fprintf(stderr, "DECNETD-E-ROUTERTEST, read2 failed\n");
        fail = 1; goto done;
    }
    st = DNET_ADJ_DOWN;
    if (dnet_engine_rx_frame(&R, now + 1, rxbuf, (size_t)n, from, &st) != 1 ||
        st != DNET_ADJ_UP) {
        fprintf(stderr, "DECNETD-E-ROUTERTEST, R did not reach UP with E (st=%d)\n",
                (int)st);
        fail = 1; goto done;
    }

done:
    close(sv[0]);
    close(sv[1]);
    if (fail) {
        printf("DECNETD-ROUTERTEST: FAIL\n");
        return 1;
    }
    printf("DECNETD-I-ROUTERTEST, router run-mode proof PASSED"
           " (router-hello emitted + node-type=router; endnode SELECTED it as DR"
           " and named it in rtr; router reached UP -- all over a socketpair, NO"
           " real node touched)\n");
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
 * ================ --set-host-src-codes-selftest (vms-15a/a70) ===============
 * The live $ SET HOST client must NOT emit the zero/zero format-2 source codes
 * real OpenVMS session control silently discards. This proves the CI the live
 * --set-host path builds -- sethost_src_codes() feeding the exact
 * dnet_cterm_sc_connect_build() call in run_set_host_loop -- carries a NONZERO
 * group AND a nonzero user sourced from the running process, and round-trips as
 * a well-formed format-2 connect to CTERM object 42. Runs anywhere: with no
 * /dev/vms the codes come from the POSIX identity fallback (still nonzero).
 */
static int run_sethost_srccode_selftest(void)
{
    uint16_t grp = 0, usr = 0;
    sethost_src_codes(&grp, &usr);
    if (grp == 0 && usr == 0) {
        printf("DECNETD-SETHOST-SRCCODES-SELFTEST: FAIL"
               " (source group/user are BOTH zero -- VMS would discard this CI)\n");
        return 1;
    }

    uint8_t sc[128];
    size_t sclen = 0;
    if (dnet_cterm_sc_connect_build(DNET_CTERM_OBJECT, "SYSTEM", grp, usr,
                                    "", "", "", sc, sizeof(sc), &sclen) != DNET_CTERM_OK) {
        printf("DECNETD-SETHOST-SRCCODES-SELFTEST: FAIL (connect build failed)\n");
        return 1;
    }

    struct dnet_cterm_sc_connect c;
    if (dnet_cterm_sc_connect_parse(sc, sclen, &c) != DNET_CTERM_OK) {
        printf("DECNETD-SETHOST-SRCCODES-SELFTEST: FAIL (connect parse failed)\n");
        return 1;
    }
    /* The CI must name CTERM object 42 (fmt-0 dst) with a fmt-2 source whose
     * group AND user are the nonzero process identity we sourced. */
    if (c.dst_object != DNET_CTERM_OBJECT ||
        c.src_format != DNET_SC_FMT_CODED ||
        c.src_grpcode == 0 || c.src_usrcode == 0 ||
        c.src_grpcode != grp || c.src_usrcode != usr) {
        printf("DECNETD-SETHOST-SRCCODES-SELFTEST: FAIL"
               " (dst_obj=%u src_fmt=%u grp=0x%04x usr=0x%04x)\n",
               c.dst_object, c.src_format, c.src_grpcode, c.src_usrcode);
        return 1;
    }

    printf("DECNETD-I-SETHOSTSRCCODES, live --set-host CI carries NONZERO"
           " format-2 source codes group=0x%04x user=0x%04x (sourced from the"
           " running process, not a template) -- VMS session control dispatches"
           " it to CTERM object 42 instead of discarding it\n",
           c.src_grpcode, c.src_usrcode);
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

/*
 * ================== $ SET HOST OUTBOUND CLIENT (rd vms-f54) ==================
 *
 * The CLIENT half of $ SET HOST: an OVMX node opens a CTERM terminal session to
 * a REMOTE node's Session Control object 42, and the remote's LOGINOUT
 * authenticates the user FRESH (the carried username is proxy/accounting only --
 * see dnet_cterm_sc_connect_build). The local interactive terminal rides the NSP
 * logical link until the remote session logs out, then control returns with the
 * canonical "%REM-S-END, control returned to node <NODE>::" (oracle
 * docs/oracle/vax-sethost-cterm.console.txt).
 *
 * ANTI-LARP TERMINAL I/O (the standing invariant for this lane): the client runs
 * in the user's interactive process, so it does its LOCAL terminal I/O through
 * its VMS terminal CHANNEL -- $ASSIGN SYS$INPUT / SYS$OUTPUT, $QIO IO$_SETMODE
 * (OVMX pass-all selector IO$K_TT_PASSALL) to hand echo/editing to the remote,
 * and $QIO IO$_READVBLK / IO$_WRITEVBLK to move bytes -- the SAME executive
 * terminal path a console login or an RTAn: device uses. It NEVER calls
 * tcsetattr/cfmakeraw on fd 0/1, and it contains NO fork/exec/openpty/dup2. The
 * termios that realises pass-all lives in the executive terminal driver
 * (src/libvms/syssvc/sys_qio.c qio_terminal_setmode), below the $QIO interface.
 */

static void sethost_mkdesc(struct dsc$descriptor_s *d, const char *s)
{
    d->dsc$w_length = (uint16_t)strlen(s);
    d->dsc$b_dtype = DSC$K_DTYPE_T;
    d->dsc$b_class = DSC$K_CLASS_S;
    d->dsc$a_pointer = (char *)s;
}

/* Set the local terminal channel's line discipline via the executive terminal
 * driver ($QIO IO$_SETMODE). passall=1 -> pass-through (remote owns echo/edit);
 * passall=0 -> restore the interactive line discipline. Off a real tty (a pipe
 * in the automated test) this is a harmless no-op in the driver. */
static void sethost_set_line(uint16_t chan, int passall)
{
    struct _iosb iosb;
    (void)sys$qiow(0, chan, IO$_SETMODE, &iosb, NULL, 0,
                   NULL, passall ? IO$K_TT_PASSALL : IO$K_TT_NORMAL,
                   0, 0, 0, 0);
}

/* Write a NUL-terminated string to the local terminal through its VMS output
 * channel ($QIO IO$_WRITEVBLK) -- e.g. the canonical %REM-S-END message. */
static void sethost_term_write(uint16_t chan, const char *s)
{
    struct _iosb iosb;
    fflush(stdout);
    (void)sys$qiow(0, chan, IO$_WRITEVBLK, &iosb, NULL, 0,
                   (void *)s, (uint32_t)strlen(s), 0, 0, 0, 0);
}

/* HELLO cadence + link give-up/retransmit tick, then flush any FSM PDU. */
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
 * to the peer's DECnet id (frame[0..5], the routing dst the FSM wrote). */
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
 * is consumed (peer HELLOs honoured), returning DNET_LINK_EV_NONE. An own-echo or
 * a frame addressed elsewhere returns NONE. -1 on a hard recv error. On
 * DNET_LINK_EV_DATA the payload is in eng->rx_data / eng->rx_datalen. */
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

    /* Is this a long-data (NSP-bearing) frame? Skip the optional Phase IV
     * intra-Ethernet pad (a leading 0x80-bit byte = (byte & 0x7f) bytes) before
     * reading the RFLG -- real VMS prepends a 0x81 pad on unicast routed data, so
     * the RFLG is NOT at a fixed offset -- and mask the intra-Ethernet flag off
     * the RFLG (a real VAX sends its Connect Confirm / data back with RFLG 0x26,
     * where the CI carried 0x2e). Getting either wrong drops every unicast reply
     * from the VAX as if it were a HELLO (a70-A: the CC was on the wire but
     * writes_recv stayed 0). */
    int is_nsp = 0;
    {
        size_t dpos = (size_t)DNET_ETH_HDRLEN + DNET_DATA_LENPREFIX;
        if ((size_t)n > dpos) {
            const uint8_t *dp = rxbuf + dpos;
            size_t drem = (size_t)n - dpos;
            size_t dpad = (drem >= 1 && (dp[0] & 0x80)) ? (size_t)(dp[0] & 0x7f) : 0;
            if (drem > dpad && DNET_RFLAG_IS_LONG_DATA(dp[dpad]))
                is_nsp = 1;
        }
    }
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

/*
 * --set-host AREA.NODE : the CTERM TERMINAL (the $ SET HOST client). Opens the
 * logical link to the peer's CTERM object (42), binds a terminal session,
 * negotiates characteristics, then bridges the LOCAL VMS terminal channel to the
 * remote session -- terminal keystrokes ($QIO read) -> CTERM Read Data, remote
 * CTERM Write -> terminal ($QIO write) -- until the host unbinds, the link drops,
 * or the run ends. It poll()s the terminal channel's fd AND the datalink for
 * READINESS (bytes still MOVE through $QIO) so the HELLO cadence + link tick keep
 * firing. On teardown control returns with the canonical %REM-S-END.
 */
/*
 * The format-2 SRCNAME group/user codes for the $ SET HOST Connect Initiate.
 *
 * vms-15a/a70: a CTERM CI whose format-2 source descriptor carries group 0 AND
 * user 0 is SILENTLY DISCARDED by real OpenVMS VAX V7.3 session control -- it
 * never reaches the CTERM (object 42) server, so no Connect Confirm returns and
 * LOGINOUT is never spawned (proven on the isolated lab: VAX1 answers our DI
 * with a DC but emits ZERO in response to the CI). Every ACCEPTED real-VAX CI
 * carries NONZERO codes (oracle /lab/decnet-wireproof/real-cterm-ci.hex: two
 * SYSTEM-sourced samples with DIFFERENT nonzero group/user -- they are the
 * SOURCE PROCESS's own identity, which is why they vary per session; they are
 * NOT a fixed constant and MENUVER 0x27 is accepted, so this was never a
 * MENUVER bug).
 *
 * So the live client must source the codes from the RUNNING PROCESS, the way
 * VMS does -- never a hardcoded template. The faithful source is the process
 * UIC the executive holds: $GETJPI(JPI$_UIC) == vms_kif_getjpi_self()->uic,
 * packed (group << 16) | member (INV-6: the value is executive state, read
 * live, never plumbed frame-to-frame). If no executive identity is stamped on
 * this process (a standalone DECNETD with no image activation / no /dev/vms),
 * fall back to the real POSIX identity of the running process (gid -> group,
 * uid -> member), and as a last resort its pid -- still the process's own
 * identity, still nonzero, never an invented constant.
 */
static void sethost_src_codes(uint16_t *grp, uint16_t *usr)
{
    uint16_t g = 0, u = 0;
    struct vms_procinfo pi;
    if ((vms_kif_getjpi_self(&pi) & 1) && pi.uic != 0) {
        g = (uint16_t)(pi.uic >> 16);      /* UIC group  */
        u = (uint16_t)(pi.uic & 0xFFFF);   /* UIC member */
    }
    if (g == 0 && u == 0) {
        g = (uint16_t)(getgid() & 0xFFFF);
        u = (uint16_t)(getuid() & 0xFFFF);
    }
    /* Never emit the zero/zero pair VMS discards. */
    if (g == 0) g = (uint16_t)(((unsigned)getpid()        & 0x7FFF) | 1);
    if (u == 0) u = (uint16_t)((((unsigned)getpid() >> 15) & 0x7FFF) | 1);
    *grp = g;
    *usr = u;
}

/*
 * Try to satisfy one outstanding host read-solicit from the buffered local
 * input queue (rd vms-6165). Returns 1 if a line was dequeued and sent as a
 * CTERM Read Data, 0 if the queue has no complete line yet (the caller should
 * remember the solicit and retry once more input arrives or EOF fires), or -1
 * if the send itself failed (same as any other link-send failure).
 */
static int sethost_send_queued_line(struct dnet_cterm_session *term,
                                    struct dnet_cterm_inq *inq,
                                    struct dnet_engine *eng, int sock,
                                    unsigned ifindex, dnet_tick_t now,
                                    uint8_t *cpdu, size_t cpdu_cap)
{
    uint8_t line[DNET_CTERM_MAX_DATA];
    size_t linelen = 0;
    if (!dnet_cterm_inq_dequeue(inq, line, sizeof(line), &linelen))
        return 0;
    size_t clen = 0;
    if (dnet_cterm_found_read_data_build(line, linelen, 0x0d, cpdu, cpdu_cap,
                                         &clen) != 0)
        return -1;
    if (cterm_link_send(eng, sock, ifindex, cpdu, clen, now) != 0)
        return -1;
    term->reads_sent++;
    return 1;
}

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
     * analog), defaulting to SYSTEM. It is NEVER read from the process
     * environment (vms-cb5 identity-environment census), and it is proxy /
     * accounting information only -- the REMOTE LOGINOUT authenticates fresh. */
    if (!user || !*user)
        user = "SYSTEM";
    uint8_t sc[128];
    size_t sclen = 0;
    /* The format-2 source group/user codes are the running process's own
     * identity (executive UIC, else POSIX id) -- NONZERO, so VMS session
     * control dispatches the CI to the CTERM server instead of discarding it
     * (vms-15a/a70). See sethost_src_codes(). */
    uint16_t src_grp = 0, src_usr = 0;
    sethost_src_codes(&src_grp, &src_usr);
    if (dnet_cterm_sc_connect_build(DNET_CTERM_OBJECT, user, src_grp, src_usr,
                                    "", "", "",
                                    sc, sizeof(sc), &sclen) != 0) {
        fprintf(stderr, "DECNETD-E-SCBUILD, CTERM connect-data build failed\n");
        return 1;
    }

    /* A process context is required for the channel table. An executive-
     * activated image already holds one; a standalone DECNETD.EXE does not --
     * establish one before $ASSIGN (the vmssshd/DCL bootstrap). */
    if (!vms_pcb_get())
        vms_pcb_init(0);

    /* Assign the LOCAL VMS terminal channels (the anti-LARP core): SYS$INPUT for
     * keystrokes, SYS$OUTPUT for screen writes. All terminal I/O goes through
     * these channels via $QIO -- never raw termios on fd 0/1. */
    uint16_t ch_in = 0, ch_out = 0;
    struct dsc$descriptor_s din, dout;
    /* The trailing ':' is what sys$assign's device resolver keys on for the
     * standard-stream logicals (src/libvms/syssvc/sys_assign.c resolve). */
    sethost_mkdesc(&din, "SYS$INPUT:");
    sethost_mkdesc(&dout, "SYS$OUTPUT:");
    if (!(sys$assign(&din, &ch_in, 0, NULL) & 1) ||
        !(sys$assign(&dout, &ch_out, 0, NULL) & 1)) {
        fprintf(stderr, "DECNETD-E-NOTERMCHAN, could not $ASSIGN the local"
                        " terminal (SYS$INPUT/SYS$OUTPUT)\n");
        if (ch_in) sys$dassgn(ch_in);
        return 1;
    }
    int term_fd = vms$$chan_to_fd(ch_in);   /* poll() readiness only */

    uint8_t frame[DNET_FRAME_MAX], rxbuf[DNET_FRAME_MAX], cpdu[DNET_CTERM_MAX_PDU];
    size_t flen = 0, clen = 0;
    dnet_tick_t now = monotonic_sec();
    if (dnet_engine_link_open(eng, parea, pnode, 0x2001, sc, sclen, 1459, 1,
                              DNET_NSP_VER_41, frame, sizeof(frame), &flen, now)
            != DNET_ENGINE_OK) {
        fprintf(stderr, "DECNETD-E-NOCONNECT, could not open a logical link"
                        " to %u.%u\n", parea, pnode);
        sys$dassgn(ch_in); sys$dassgn(ch_out);
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

    int passall_on = 0, stdin_eof = 0, done = 0, rc = 0, session_bound_ever = 0;
    /* rd vms-6165: CTERM input is PROMPT-DRIVEN. Local stdin is buffered here,
     * never dumped on BOUND, and released ONE LINE PER HOST SOLICIT (a 02-08
     * TK_START_READ). `read_pending` remembers a solicit that arrived before
     * the queue had a complete line, so the next terminal read (or EOF) can
     * satisfy it immediately instead of waiting for another solicit that will
     * never come (the host is already blocked in its own read). */
    struct dnet_cterm_inq inq;
    dnet_cterm_inq_init(&inq);
    int read_pending = 0;

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
        if (dnet_cterm_is_bound(&term) && !stdin_eof && term_fd >= 0) {
            pfd[nfd].fd = term_fd;   pfd[nfd].events = POLLIN; pfd[nfd].revents = 0; nfd++;
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
                       " sending NSP link-service (credit) then awaiting host"
                       " foundation\n", parea, pnode);
                fflush(stdout);
                /* rd vms-6165: NSP requires the INITIATOR to send a LINK SERVICE
                 * right after the CC -- it acks the CC + opens the flow-control
                 * window, and ONLY THEN does a real VAX send its foundation
                 * data. Without it VAX1 loops re-sending the CC (writes_recv=0).
                 * This is the NSP layer; foundation CONTENT stays host-first. */
                {
                    uint8_t lsf[DNET_FRAME_MAX];
                    size_t lslen = 0;
                    if (dnet_engine_link_service(eng, lsf, sizeof(lsf), &lslen, now)
                            == DNET_ENGINE_OK) {
                        uint8_t dst[DNET_ADDR_LEN];
                        memcpy(dst, lsf, DNET_ADDR_LEN);   /* routing dst the FSM wrote */
                        scs_datalink_send(sock, (int)ifindex, DNET_ETHERTYPE,
                                          dst, lsf, lslen);
                    } else {
                        fprintf(stderr, "DECNETD-E-LINKSVC, could not send NSP"
                                        " link-service credit grant\n");
                        rc = 1; done = 1;
                        break;
                    }
                }
                /* Arm the CTERM client FSM and WAIT -- the host sends its
                 * foundation seg-1; the DATA handler drives the client replies. */
                if (dnet_cterm_client_open(&term) != 0) {
                    fprintf(stderr, "DECNETD-E-CTERMOPEN, could not arm CTERM"
                                    " client foundation\n");
                    rc = 1; done = 1;
                }
                break;
            case DNET_LINK_EV_DATA: {
                if (dnet_cterm_state_of(&term) == DNET_CTERM_S_BINDING) {
                    /* FOUNDATION PHASE (rd vms-6165): consume the host's
                     * foundation message, then drain every client reply now due
                     * (client seg-1, then the seg-2/3/4 burst). WIDTH/PAGE are
                     * the captured 132/24 so every emitted byte is oracle-exact. */
                    int prog = 0;
                    if (dnet_cterm_client_found_rx(&term, eng->rx_data,
                                                   eng->rx_datalen, &prog)
                            != DNET_CTERM_OK)
                        break;
                    for (;;) {
                        int frc = dnet_cterm_client_found_next(&term, 132, 24,
                                                               cpdu, sizeof(cpdu),
                                                               &clen);
                        if (frc != DNET_CTERM_OK || clen == 0)
                            break;
                        if (cterm_link_send(eng, sock, ifindex, cpdu, clen, now) != 0) {
                            fprintf(stderr, "DECNETD-E-FOUND, could not send a"
                                            " CTERM foundation reply\n");
                            rc = 1; done = 1;
                            break;
                        }
                    }
                    if (!done && dnet_cterm_is_bound(&term) && !session_bound_ever) {
                        session_bound_ever = 1;
                        log_ts(stdout);
                        printf(" DECNETD-I-BOUND, CTERM foundation negotiated --"
                               " terminal session BOUND on circuit %s\n",
                               eng->circuit);
                        fflush(stdout);
                        /* Hand echo/editing to the REMOTE session: pass-all the
                         * LOCAL terminal through the executive driver. */
                        sethost_set_line(ch_in, 1);
                        passall_on = 1;
                    }
                    break;
                }

                /* BOUND: real terminal I/O, all inside 09-envelopes. */
                enum dnet_cterm_found_term_kind tk = DNET_CTERM_TK_NONE;
                uint8_t txt[DNET_CTERM_MAX_DATA];
                size_t txtlen = 0;
                uint8_t rhandle[2] = { 0, 0 };
                if (dnet_cterm_found_terminal_rx(eng->rx_data, eng->rx_datalen,
                                                 &tk, txt, sizeof(txt), &txtlen,
                                                 rhandle) != DNET_CTERM_OK)
                    break;
                if (tk == DNET_CTERM_TK_WRITE) {
                    term.writes_recv++;
                    if (txtlen) {
                        struct _iosb iosb;
                        (void)sys$qiow(0, ch_out, IO$_WRITEVBLK, &iosb, NULL, 0,
                                       txt, (uint32_t)txtlen, 0, 0, 0, 0);
                    }
                } else if (tk == DNET_CTERM_TK_START_READ) {
                    /* rd vms-6165: a 02-08 is PROMPT-AND-READ -- display the
                     * prompt text, THEN answer with exactly one queued input
                     * line. One solicit -> one line, never more, never before
                     * this arrives. If the queue has no complete line yet
                     * (interactive typing still in flight), remember the
                     * solicit and satisfy it the moment one becomes available. */
                    term.writes_recv++;
                    if (txtlen) {
                        struct _iosb iosb;
                        (void)sys$qiow(0, ch_out, IO$_WRITEVBLK, &iosb, NULL, 0,
                                       txt, (uint32_t)txtlen, 0, 0, 0, 0);
                    }
                    int srr = sethost_send_queued_line(&term, &inq, eng, sock,
                                                       ifindex, now, cpdu,
                                                       sizeof(cpdu));
                    if (srr < 0) {
                        fprintf(stderr, "DECNETD-E-READDATA, could not send"
                                        " CTERM Read Data\n");
                        rc = 1; done = 1;
                    } else if (srr == 0) {
                        read_pending = 1;
                    } else {
                        read_pending = 0;
                    }
                } else if (tk == DNET_CTERM_TK_READ_ATTR) {
                    /* Host solicited terminal characteristics: answer with the
                     * oracle read-characteristics reply, echoing its handle. */
                    if (dnet_cterm_found_client_readchar_build(rhandle, cpdu,
                                                               sizeof(cpdu),
                                                               &clen) == 0)
                        cterm_link_send(eng, sock, ifindex, cpdu, clen, now);
                }
                /* TK_OTHER / TK_NONE: NSP-ack only (dnet_recv_route already did),
                 * nothing to display or answer. */
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
            struct _iosb iosb;
            /* Read the keystrokes through the VMS terminal channel ($QIO), never
             * a raw read on fd 0. poll() above only told us bytes are ready. */
            uint32_t rst = sys$qiow(0, ch_in, IO$_READVBLK, &iosb, NULL, 0,
                                    inbuf, (uint32_t)sizeof(inbuf), 0, 0, 0, 0);
            uint32_t rn = (rst & 1) ? iosb.iosb$l_dev_depend : 0;
            if ((rst & 1) && rn > 0) {
                /* rd vms-6165: local keystrokes are BUFFERED, never sent
                 * immediately -- CTERM input is prompt-gated (see the queue's
                 * doc comment in dnet_cterm.h). Only actually emit a Read Data
                 * if a host solicit is already outstanding (read_pending). */
                (void)dnet_cterm_inq_feed(&inq, inbuf, (size_t)rn);
            } else {
                /* Local EOF (SS$_ENDOFFILE) or a channel error: stop polling
                 * for more input but KEEP THE LINK UP -- whatever is already
                 * queued (a canned/redirected stdin fully read at once) is
                 * still dequeued one line per solicit, and the host's
                 * remaining output still drains. The session ends on the
                 * host's Unbind, a link drop, or --duration -- NEVER on
                 * stdin EOF by itself (rd vms-6165 lab iter 2: tearing down
                 * here is what caused the blast-then-quit bug). */
                stdin_eof = 1;
                dnet_cterm_inq_eof(&inq);
                log_ts(stdout);
                printf(" DECNETD-I-EOF, local input closed -- %zu byte(s) still"
                       " queued, draining remote output on circuit %s\n",
                       inq.len, eng->circuit);
                fflush(stdout);
            }
            /* A host solicit may already be waiting on a line that was not
             * available yet -- satisfy it now if the queue (or EOF) supplied
             * one. Loop: EOF can make several trailing lines available, but a
             * solicit is only EVER outstanding one at a time (the host waits
             * for our reply before prompting again), so this fires at most
             * once per solicit. */
            if (read_pending) {
                int srr = sethost_send_queued_line(&term, &inq, eng, sock,
                                                   ifindex, now, cpdu,
                                                   sizeof(cpdu));
                if (srr < 0) {
                    fprintf(stderr, "DECNETD-E-READDATA, could not send"
                                    " CTERM Read Data\n");
                    rc = 1; done = 1;
                } else if (srr == 1) {
                    read_pending = 0;
                }
            }
        }
    }

    /* Restore the local terminal's interactive line discipline through the
     * executive ($QIO IO$_SETMODE), before control returns to DCL. */
    if (passall_on)
        sethost_set_line(ch_in, 0);

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

    /* CONTROL RETURNS with the canonical VMS message (oracle
     * docs/oracle/vax-sethost-cterm.console.txt): the LOCAL node is the node
     * control returns to. Written through the terminal's VMS output channel,
     * only once a session was actually established. */
    if (session_bound_ever) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "%%REM-S-END, control returned to node %s::\n", eng->node_name);
        sethost_term_write(ch_out, msg);
    }

    log_ts(stdout);
    printf(" DECNETD-I-SETHOSTEND, SET HOST session ended: cterm writes_recv=%lu"
           " reads_sent=%lu on circuit %s\n",
           term.writes_recv, term.reads_sent, eng->circuit);
    fflush(stdout);

    sys$dassgn(ch_in);
    sys$dassgn(ch_out);
    return rc;
}

/*
 * ============== --fal-selftest / --fal-accept-test (rd vms-8c2) ==============
 * The DECnet FILE ACCESS LISTENER (object 17) + the COPY node:: client, proven
 * over a real NSP logical link (Ethernet + Phase IV routing header + NSP PDU)
 * moved over a socketpair(2) -- the same wire path DECNETD uses on the live
 * datalink, with the two blocking peers (FAL server + COPY client) running in
 * two threads so their choreography is real, not stepped by the test.
 *
 * WHAT EACH PROVES, AND WHERE IT IS A HARD GATE:
 *   --fal-selftest      NO executive needed, runs anywhere (the honest floor,
 *                       like vms-b19 for SET HOST): (A) a COPY client emits a
 *                       real object-17 Connect Initiate CARRYING the access-
 *                       control username+password, and FAL REFUSES it with an
 *                       NSP Disconnect when the credentials cannot be
 *                       authenticated (no /dev/vms -> sysuaf_lookup fails ->
 *                       SS$_INVLOGIN -> reject); (B) the DAP-over-NSP transport
 *                       pump itself -- CONFIGURATION exchange + ACCESS + an
 *                       honest STATUS(access-failed) for a missing file --
 *                       round-trips end to end over the threaded socketpair.
 *   --fal-accept-test   The FULL transfer, a HARD GATE wherever /dev/vms + the
 *                       mounted ODS-2 SYSUAF are present (the booted image):
 *                       real SYSUAF/Purdy auth (GUEST/GUEST accepted, a wrong
 *                       password REFUSED, DISABLED refused by DISUSER), then a
 *                       sequential file transferred BOTH directions (PUT then
 *                       GET) through real DAP over the link and real RMS over
 *                       the ACP, byte-verified. It FAILS honestly where the
 *                       executive/SYSUAF is absent (INV-6) -- it does not
 *                       degrade to a stub.
 */
struct fal_xport {
    struct dnet_engine *eng;   /* this end's engine (owns the one link)         */
    int      wfd, rfd;         /* this end's socketpair descriptors             */
    dnet_tick_t *tick;         /* shared monotonic tick (per test run)          */
};

/* Ship one DAP message as an NSP data segment on the link. */
static int fal_xport_send(void *ctx, const struct dnet_dap_msg *m)
{
    struct fal_xport *x = ctx;
    uint8_t dap[DNET_DAP_MAX_MSG], frame[DNET_FRAME_MAX];
    size_t  daplen = 0, flen = 0;
    if (dnet_dap_encode(m, dap, sizeof dap, &daplen) != DNET_DAP_OK) return -1;
    if (dnet_engine_link_send(x->eng, dap, daplen, frame, sizeof frame, &flen,
                              (*x->tick)++) != 0)
        return -1;
    if (write(x->wfd, frame, flen) != (ssize_t)flen) return -1;
    return 0;
}

/* Receive the next DAP message. Absorbs NSP acks and ships the ack owed for a
 * received data segment (real NSP flow), so the caller sees only DAP messages.
 * Returns 0 with *m filled, or -1 on a closed link / decode failure. */
static int fal_xport_recv(void *ctx, struct dnet_dap_msg *m)
{
    struct fal_xport *x = ctx;
    uint8_t rxbuf[DNET_FRAME_MAX], reply[DNET_FRAME_MAX];
    for (;;) {
        ssize_t n = read(x->rfd, rxbuf, sizeof rxbuf);
        if (n <= 0) return -1;
        size_t rlen = 0; int has_reply = 0;
        enum dnet_link_event ev = DNET_LINK_EV_NONE;
        if (dnet_engine_link_rx(x->eng, (*x->tick)++, rxbuf, (size_t)n,
                                reply, sizeof reply, &rlen, &has_reply, &ev) != 0)
            return -1;
        if (has_reply && write(x->wfd, reply, rlen) != (ssize_t)rlen) return -1;
        if (ev == DNET_LINK_EV_DATA) {
            size_t consumed = 0;
            if (dnet_dap_decode(x->eng->rx_data, x->eng->rx_datalen, m, &consumed)
                != DNET_DAP_OK)
                return -1;
            return 0;
        }
        if (ev == DNET_LINK_EV_DISCONNECT || ev == DNET_LINK_EV_DISCONNECT_CONF)
            return -1;
        /* ACK / NONE / connect events: absorb and keep reading. */
    }
}

/*
 * Bring up an object-17 link L->R carrying the access-control creds, and run
 * FAL's connect-time auth gate on R. Returns 0 and leaves the link UP (both
 * ends) when auth PASSED and R accepted; returns 1 (link refused, R sent a
 * Disconnect Initiate that L saw) when auth FAILED; -1 on a wire error.
 */
static int fal_bringup(struct dnet_engine *L, struct dnet_engine *R,
                       int sv0, int sv1, dnet_tick_t *tick,
                       const char *user, const char *pass, uint32_t *auth_out)
{
    uint8_t conn[128], frame[DNET_FRAME_MAX], rxbuf[DNET_FRAME_MAX], reply[DNET_FRAME_MAX];
    size_t  clen = 0, flen = 0, rxlen = 0, rlen = 0;
    int has_reply = 0;
    enum dnet_link_event ev = DNET_LINK_EV_NONE;

    /* The COPY client puts the NODE"user pw":: access string on the connect in
     * the oracle's tag positions (object 17, format-0 dst; format-2 src; the
     * access-control userid/password/account) via the proven builder. */
    if (dnet_cterm_sc_connect_build(DNET_OBJ_FAL, "OVMXL", 0x021a, 0x2020,
                                    user, pass, "", conn, sizeof conn, &clen) != 0)
        return -1;
    if (dnet_engine_link_open(L, 1, 11, 0x2001, conn, clen, 1459, 1,
                              DNET_NSP_VER_41, frame, sizeof frame, &flen, (*tick)++) != 0 ||
        move_frame(sv0, sv1, frame, flen, rxbuf, sizeof rxbuf, &rxlen) != 0 ||
        dnet_engine_link_rx(R, (*tick)++, rxbuf, rxlen, reply, sizeof reply, &rlen,
                            &has_reply, &ev) != 0 || ev != DNET_LINK_EV_CONNECT_IND)
        return -1;

    /* R is FAL: authenticate the connect BEFORE accepting (INV-6 -- no file is
     * served on an unauthenticated connect). Bounded credentials never wiped
     * before use are the FAL analogue of the CTERM no-auth gate. */
    char who[DNET_FAL_USER_MAX + 1];
    uint32_t auth = dnet_fal_connect_auth(R->link.conn_data, R->link.conn_len,
                                          who, sizeof who);
    if (auth_out) *auth_out = auth;

    if (auth != SS$_NORMAL) {
        /* Refuse: Disconnect Initiate (object rejected connect), L sees it. */
        if (dnet_engine_link_close(R, DNET_LINK_REASON_OBJREJ, reply, sizeof reply,
                                   &rlen, (*tick)++) != 0 ||
            move_frame(sv1, sv0, reply, rlen, rxbuf, sizeof rxbuf, &rxlen) != 0 ||
            dnet_engine_link_rx(L, (*tick)++, rxbuf, rxlen, frame, sizeof frame,
                                &flen, &has_reply, &ev) != 0)
            return -1;
        return 1;   /* honest refusal proven */
    }

    /* Auth OK: accept -> Connect Confirm -> L sees the link RUN. */
    if (dnet_engine_link_accept(R, 0x2002, reply, sizeof reply, &rlen, (*tick)++) != 0 ||
        move_frame(sv1, sv0, reply, rlen, rxbuf, sizeof rxbuf, &rxlen) != 0 ||
        dnet_engine_link_rx(L, (*tick)++, rxbuf, rxlen, frame, sizeof frame, &flen,
                            &has_reply, &ev) != 0 || ev != DNET_LINK_EV_CONNECT_CONF ||
        !dnet_link_is_up(&L->link) || !dnet_link_is_up(&R->link))
        return -1;
    return 0;   /* link UP */
}

/* Thread body: the FAL server side of one accepted session. */
struct fal_server_arg { struct fal_xport xp; uint32_t status; };
static void *fal_server_thread(void *v)
{
    struct fal_server_arg *a = v;
    struct dnet_dap_transport t = { fal_xport_send, fal_xport_recv, &a->xp };
    a->status = dnet_fal_server_run(&t);
    return NULL;
}

static int run_fal_selftest(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        fprintf(stderr, "DECNETD-E-FALSELF, socketpair failed: %s\n", strerror(errno));
        return 1;
    }
    const uint8_t hwL[6] = { 0x02,0,0,0,0,0x0a };
    const uint8_t hwR[6] = { 0x02,0,0,0,0,0x0b };
    struct dnet_engine L, R;
    if (dnet_engine_init(&L, 1, 10, "OVMXL", "EWA0", NULL, hwL, 0, 0, 0) != 0 ||
        dnet_engine_init(&R, 1, 11, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0) != 0) {
        fprintf(stderr, "DECNETD-E-FALSELF, engine init failed\n");
        close(sv[0]); close(sv[1]); return 1;
    }
    int pass = 0, fail = 0;
    dnet_tick_t tick = 100;

    /* (A) THE HONEST FLOOR: a real object-17 connect carrying creds is REFUSED
     * with an NSP disconnect when the credentials cannot be authenticated (no
     * executive here, so sysuaf_lookup fails -> SS$_INVLOGIN). */
    uint32_t auth = 0;
    int br = fal_bringup(&L, &R, sv[0], sv[1], &tick, "GUEST", "GUEST", &auth);
    if (br == 1 && auth != SS$_NORMAL) {
        printf("DECNETD-I-FALSELF, object-17 connect carried the access-control"
               " creds and FAL REFUSED it (status %08X) with an NSP disconnect --"
               " no file served on an unauthenticated connect (INV-6)\n", auth);
        pass++;
    } else {
        printf("DECNETD-E-FALSELF, expected an honest refusal of the unauthenticated"
               " connect, got bringup=%d auth=%08X\n", br, auth);
        fail++;
    }

    /* (B) THE TRANSPORT PUMP: bring a link UP bypassing the auth gate (this half
     * proves the DAP-over-NSP threaded transport, not auth), and run a GET of a
     * file the server cannot open (no ACP volume here) -- CONFIGURATION + ACCESS
     * + an honest STATUS(access-failed) must round-trip end to end, and both
     * peers return the honest miss. Fresh engines + a fresh socketpair: sub-test
     * A left its engines with a closed (rejected) link. */
    int sv2[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv2) != 0) { close(sv[0]); close(sv[1]); return 1; }
    struct dnet_engine L2, R2;
    dnet_engine_init(&L2, 1, 10, "OVMXL", "EWA0", NULL, hwL, 0, 0, 0);
    dnet_engine_init(&R2, 1, 11, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0);
    uint8_t frame[DNET_FRAME_MAX], rxbuf[DNET_FRAME_MAX], reply[DNET_FRAME_MAX];
    size_t flen = 0, rxlen = 0, rlen = 0; int has_reply = 0;
    enum dnet_link_event ev = DNET_LINK_EV_NONE;
    if (dnet_engine_link_open(&L2, 1, 11, 0x2003, NULL, 0, 1459, 1, DNET_NSP_VER_41,
                              frame, sizeof frame, &flen, tick++) == 0 &&
        move_frame(sv2[0], sv2[1], frame, flen, rxbuf, sizeof rxbuf, &rxlen) == 0 &&
        dnet_engine_link_rx(&R2, tick++, rxbuf, rxlen, reply, sizeof reply, &rlen,
                            &has_reply, &ev) == 0 && ev == DNET_LINK_EV_CONNECT_IND &&
        dnet_engine_link_accept(&R2, 0x2004, reply, sizeof reply, &rlen, tick++) == 0 &&
        move_frame(sv2[1], sv2[0], reply, rlen, rxbuf, sizeof rxbuf, &rxlen) == 0 &&
        dnet_engine_link_rx(&L2, tick++, rxbuf, rxlen, frame, sizeof frame, &flen,
                            &has_reply, &ev) == 0 && dnet_link_is_up(&L2.link)) {
        struct fal_server_arg sarg = { { &R2, sv2[1], sv2[1], &tick }, 0 };
        pthread_t th;
        if (pthread_create(&th, NULL, fal_server_thread, &sarg) == 0) {
            struct fal_xport cxp = { &L2, sv2[0], sv2[0], &tick };
            struct dnet_dap_transport ct = { fal_xport_send, fal_xport_recv, &cxp };
            uint32_t cst = dnet_fal_client_get("OVMXR::DKA0:[X]NOPE.TXT",
                                               "DKA0:[X]LOCAL.TXT", &ct);
            pthread_join(th, NULL);
            if (cst == SS$_NOSUCHFILE && sarg.status == SS$_NOSUCHFILE) {
                printf("DECNETD-I-FALSELF, the DAP-over-NSP transport pump round-trips"
                       " end to end over the threaded socketpair: CONFIGURATION +"
                       " ACCESS + honest STATUS(access-failed) for a missing file,"
                       " both peers return the honest miss\n");
                pass++;
            } else {
                printf("DECNETD-E-FALSELF, transport pump did not return the honest"
                       " miss (client %08X server %08X)\n", cst, sarg.status);
                fail++;
            }
        } else { fail++; }
    } else {
        printf("DECNETD-E-FALSELF, could not bring the pump-test link up\n");
        fail++;
    }
    close(sv2[0]); close(sv2[1]);

    close(sv[0]); close(sv[1]);
    printf("DECNETD-I-FALSELF, %d passed, %d failed\n", pass, fail);
    if (fail == 0 && pass == 2) { printf("DECNETD-FAL-SELFTEST: PASS\n"); return 0; }
    printf("DECNETD-FAL-SELFTEST: FAIL\n");
    return 1;
}

/* Compare a stored ODS-2 file's records to an expected multi-line body.
 * Returns 1 on an exact match. Reads through RMS over the ACP (real file I/O). */
static int fal_file_matches(const char *spec, const char *const *lines, int nlines)
{
    rms_textfile_t *tf = rms_textfile_open(spec);
    if (!tf) return 0;
    char buf[DNET_DAP_MAX_REC]; int too_long = 0, i = 0, ok = 1;
    while (rms_textfile_getline(tf, buf, sizeof buf, &too_long)) {
        if (i >= nlines || strcmp(buf, lines[i]) != 0) { ok = 0; break; }
        i++;
    }
    rms_textfile_close(tf);
    return ok && i == nlines;
}

static int run_fal_accept_test(void)
{
    printf("DECNETD-I-FALACCEPT, inbound FAL (object 17) COPY -> real SYSUAF auth"
           " -> DAP/RMS transfer both directions (rd vms-8c2; oracle"
           " docs/oracle/vax-copy-fal-dap.*)\n");
    int pass = 0, fail = 0;
/* Emit a labelled line on BOTH outcomes (the house style, matching CT_CHECK):
 * the booted battery greps each assertion's PROPERTY message (a wrong password
 * REFUSED, DISABLED refused, records BYTE-MATCH) as positive evidence the
 * property was exercised, so a PASS must print its label too -- a fail-only
 * print left those greps satisfiable only when the assertion FAILED (inverted). */
#define FA_CHECK(c, msg) do { if (c) { pass++; printf("  PASS: %s\n", msg); } \
    else { fail++; printf("  FAIL: %s\n", msg); } } while (0)

    /* 1) AUTH IS REAL (the security core): the same SYSUAF/Purdy path LOGINOUT
     * uses. A fake would pass the wrong password; only a real Purdy verify
     * against the stored quadword refuses it. Fixtures are the shipped seed
     * accounts (tools/mksysuaf.c): GUEST/GUEST valid; DISABLED/DISABLED valid
     * password but DISUSER. */
    FA_CHECK(dnet_fal_authenticate("GUEST", "GUEST") == SS$_NORMAL,
             "GUEST with the correct password authenticates (real SYSUAF/Purdy)");
    FA_CHECK(dnet_fal_authenticate("GUEST", "WRONGPW") == SS$_INVLOGIN,
             "GUEST with a WRONG password is REFUSED (SS$_INVLOGIN) -- a fake would pass it");
    FA_CHECK(dnet_fal_authenticate("NOSUCHUSER99", "x") == SS$_INVLOGIN,
             "a nonexistent account is refused, indistinguishably from a bad password");
    FA_CHECK(dnet_fal_authenticate("DISABLED", "DISABLED") == SS$_NOPRIV,
             "DISABLED (correct password, DISUSER) is REFUSED -- a right password is not sufficient");

    const uint8_t hwL[6] = { 0x02,0,0,0,0,0x0a };
    const uint8_t hwR[6] = { 0x02,0,0,0,0,0x0b };

    /* 2) A COPY with a BAD password is REFUSED at connect over a real link
     * (NSP disconnect, no session, no file). */
    {
        int sv[2]; socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
        struct dnet_engine L, R; dnet_tick_t tick = 100; uint32_t auth = 0;
        dnet_engine_init(&L, 1, 10, "OVMXL", "EWA0", NULL, hwL, 0, 0, 0);
        dnet_engine_init(&R, 1, 11, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0);
        int br = fal_bringup(&L, &R, sv[0], sv[1], &tick, "GUEST", "WRONGPW", &auth);
        FA_CHECK(br == 1 && auth == SS$_INVLOGIN,
                 "a COPY with a BAD password is REFUSED with an NSP disconnect (no file served)");
        close(sv[0]); close(sv[1]);
    }

    /* 3) A COPY with the CORRECT creds transfers a sequential file BOTH
     * directions, byte-verified through real RMS over the ACP. */
    static const char *src_lines[] = {
        "Hello from OVMXL node 1.10 - DAP/FAL transfer line one",
        "Second line for a multi-record DAP data transfer",
        "Third and final record"
    };
    const int nsrc = 3;
    const char *SRC  = "SYS$SYSROOT:[SYSMGR]OVMXFAL_S.TXT";
    const char *DEST = "SYS$SYSROOT:[SYSMGR]OVMXFAL_D.TXT";
    const char *BACK = "SYS$SYSROOT:[SYSMGR]OVMXFAL_B.TXT";

    /* Lay down the source file on the ODS-2 volume via RMS. */
    int src_ok = (rms_textfile_write_line(SRC, src_lines[0]) == 0) &&
                 (rms_textfile_append_line(SRC, src_lines[1]) == 0) &&
                 (rms_textfile_append_line(SRC, src_lines[2]) == 0);
    FA_CHECK(src_ok, "source file created on the ODS-2 volume via RMS over the ACP");

    /* PUT: L copies SRC to the remote FAL, which stores it as DEST. */
    if (src_ok) {
        int sv[2]; socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
        struct dnet_engine L, R; dnet_tick_t tick = 200; uint32_t auth = 0;
        dnet_engine_init(&L, 1, 10, "OVMXL", "EWA0", NULL, hwL, 0, 0, 0);
        dnet_engine_init(&R, 1, 11, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0);
        int br = fal_bringup(&L, &R, sv[0], sv[1], &tick, "GUEST", "GUEST", &auth);
        FA_CHECK(br == 0 && auth == SS$_NORMAL, "PUT: GUEST/GUEST connect accepted, link UP");
        if (br == 0) {
            struct fal_server_arg sarg = { { &R, sv[1], sv[1], &tick }, 0 };
            pthread_t th; pthread_create(&th, NULL, fal_server_thread, &sarg);
            struct fal_xport cxp = { &L, sv[0], sv[0], &tick };
            struct dnet_dap_transport ct = { fal_xport_send, fal_xport_recv, &cxp };
            uint32_t cst = dnet_fal_client_put(SRC, DEST, &ct);
            pthread_join(th, NULL);
            FA_CHECK(cst == SS$_NORMAL && sarg.status == SS$_NORMAL,
                     "PUT: DAP transfer completed on both peers");
            FA_CHECK(fal_file_matches(DEST, src_lines, nsrc),
                     "PUT: the STORED file's records BYTE-MATCH the source (real transfer)");
        }
        close(sv[0]); close(sv[1]);
    }

    /* GET: L copies DEST back from the remote FAL into BACK; byte-verify. */
    {
        int sv[2]; socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
        struct dnet_engine L, R; dnet_tick_t tick = 300; uint32_t auth = 0;
        dnet_engine_init(&L, 1, 10, "OVMXL", "EWA0", NULL, hwL, 0, 0, 0);
        dnet_engine_init(&R, 1, 11, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0);
        int br = fal_bringup(&L, &R, sv[0], sv[1], &tick, "GUEST", "GUEST", &auth);
        FA_CHECK(br == 0 && auth == SS$_NORMAL, "GET: GUEST/GUEST connect accepted, link UP");
        if (br == 0) {
            struct fal_server_arg sarg = { { &R, sv[1], sv[1], &tick }, 0 };
            pthread_t th; pthread_create(&th, NULL, fal_server_thread, &sarg);
            struct fal_xport cxp = { &L, sv[0], sv[0], &tick };
            struct dnet_dap_transport ct = { fal_xport_send, fal_xport_recv, &cxp };
            uint32_t cst = dnet_fal_client_get(DEST, BACK, &ct);
            pthread_join(th, NULL);
            FA_CHECK(cst == SS$_NORMAL && sarg.status == SS$_NORMAL,
                     "GET: DAP transfer completed on both peers");
            FA_CHECK(fal_file_matches(BACK, src_lines, nsrc),
                     "GET: the FETCHED file's records BYTE-MATCH the source (real transfer)");
        }
        close(sv[0]); close(sv[1]);
    }

    printf("DECNETD-I-FALACCEPT, %d passed, %d failed\n", pass, fail);
    if (fail == 0 && pass > 0) { printf("DECNETD-FAL-ACCEPT: PASS\n"); return 0; }
    printf("DECNETD-FAL-ACCEPT: FAIL\n");
    return 1;
#undef FA_CHECK
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--address AREA.NODE] [options]\n"
        "  --address A.N       DECnet Phase IV executor address (1..63 . 1..1023).\n"
        "                      If omitted it is SELF-SOURCED from the node's DECnet\n"
        "                      configuration (executor.dat, written by NCP SET/DEFINE\n"
        "                      EXECUTOR ADDRESS) -- the way STARTNET.COM starts the\n"
        "                      persistent daemon with no argv. No identity is ever\n"
        "                      invented; with neither the flag nor a configured\n"
        "                      executor the daemon exits (INV-6).\n"
        "  --name NAME         NCP node name (1..6 chars; default OVMX)\n"
        "  --iface IFNAME      datalink interface (default %s)\n"
        "  --device DEV        VMS device label for the circuit (default EWA0)\n"
        "  --circuit CIRC      DECnet circuit name (default derived, e.g. EWA-0)\n"
        "  --hello-interval N  HELLO cadence T3 seconds (default %u, oracle vms-3be)\n"
        "  --router            run as a Phase IV L1 ROUTER: advertise node-type\n"
        "                      router and emit spec-faithful router-hellos to the\n"
        "                      all-endnodes multicast, so an endnode selects this\n"
        "                      node as its designated router (rd vms-0a9)\n"
        "  --priority N        with --router: DR-election priority 0..255 (default\n"
        "                      %u, the DNA-documented default)\n"
        "  --duration N        run N seconds then exit (default: until SIGINT/TERM)\n"
        "  --show-executor     print the NCP executor summary and exit (no socket)\n"
        "  --self-test         run the in-process tx/rx/adjacency proof and exit\n"
        "                      (add --router for the router run-mode isolation\n"
        "                      proof: emit->decode->endnode DR-selection over a\n"
        "                      socketpair, no netdev, NO real node -- rd vms-0a9)\n"
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
        "  --set-host-src-codes-selftest  prove the live --set-host CI carries\n"
        "                      NONZERO format-2 source group/user codes sourced\n"
        "                      from the running process (vms-15a/a70: zero/zero is\n"
        "                      silently discarded by real VMS session control)\n"
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
        "                      this daemon spawns nothing and knows no password.\n"
        "                      This is the DEFAULT for the persistent endnode\n"
        "                      daemon (NETACP serves object 42); the flag is kept\n"
        "                      for an explicit ROUTER that should also serve.\n"
        "  --no-cterm-server   do NOT serve inbound $ SET HOST -- route only. For a\n"
        "                      routing/capture invocation that wants no LOGINOUT\n"
        "                      surface. (A --router or --set-host invocation is\n"
        "                      already routing/client-only unless serve is pinned.)\n"
        "  --set-host A.N      $ SET HOST CLIENT: open a CTERM terminal session\n"
        "                      to Session Control object 42 on remote node A.N and\n"
        "                      bridge THIS process's VMS terminal channel to it\n"
        "                      ($ASSIGN SYS$INPUT/SYS$OUTPUT + $QIO -- never raw\n"
        "                      termios). The remote LOGINOUT authenticates fresh;\n"
        "                      control returns with %%REM-S-END on LOGOUT.\n"
        "  --user NAME         with --set-host: CTERM access-control username\n"
        "                      (default SYSTEM; proxy/accounting only -- the\n"
        "                      remote authenticates fresh; never from the env).\n"
        "  --fal-selftest      run the FAL/COPY honest-floor proof and exit (no\n"
        "                      executive needed): a COPY client emits a real\n"
        "                      object-17 connect carrying the access-control\n"
        "                      creds and FAL REFUSES it with an NSP disconnect\n"
        "                      when they cannot be authenticated; and the\n"
        "                      DAP-over-NSP transport pump round-trips over a\n"
        "                      threaded socketpair (rd vms-8c2)\n"
        "  --fal-accept-test   run the FULL inbound-FAL COPY proof and exit (a\n"
        "                      HARD GATE on /dev/vms + the mounted SYSUAF): real\n"
        "                      SYSUAF/Purdy auth (bad password REFUSED), then a\n"
        "                      sequential file transferred BOTH directions\n"
        "                      through real DAP + RMS over the ACP, byte-verified\n",
        argv0, DECNETD_DEFAULT_IFACE, (unsigned)DNET_T3_DEFAULT,
        (unsigned)DNET_ROUTER_PRIORITY_DEFAULT);
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
    int sethost_srccode_test = 0;
    int cterm_accept_test = 0;
    int isolation_test = 0;
    /* The persistent node daemon (NETACP) SERVES inbound $ SET HOST by default
     * -- serving object 42 is what a DECnet ancillary control process does, and
     * RUN/DETACHED (VMS semantics: an image parameter, never argv) cannot pass a
     * mode flag to the detached daemon SYS$MANAGER:STARTNET.COM starts, exactly
     * as TCPIP$STARTUP starts TCPIP$INETD with no args and it reads its own
     * SYS$SYSTEM:TCPIP$SERVICE.DAT (rd vms-a70 direction B). Serving is a strict
     * SUPERSET of routing: it is dormant until a peer sends an object-42 connect,
     * and mints nothing without the executive (fail-honest, INV-6). A ROUTER is
     * routing-only unless it is told otherwise, and --no-cterm-server forces it
     * off for a routing/capture invocation. */
    int cterm_server = 1;
    int cterm_server_explicit = 0;        /* did the caller pin serve on/off?      */
    const char *set_host_to = NULL;       /* --set-host A.N : CTERM terminal client */
    const char *set_host_user = "SYSTEM"; /* --user : CTERM access-control name      */
    int fal_self_test = 0;
    int fal_accept_test = 0;
    int router_mode = 0;           /* --router: emit router-hellos, advertise L1 router */
    int router_priority = 0;       /* --priority: DR-election priority (0 => DNA default 64) */

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
        else if (!strcmp(argv[i], "--set-host-src-codes-selftest")) sethost_srccode_test = 1;
        else if (!strcmp(argv[i], "--cterm-accept-test")) cterm_accept_test = 1;
        else if (!strcmp(argv[i], "--isolation-test")) isolation_test = 1;
        else if (!strcmp(argv[i], "--cterm-server")) { cterm_server = 1; cterm_server_explicit = 1; }
        else if (!strcmp(argv[i], "--no-cterm-server")) { cterm_server = 0; cterm_server_explicit = 1; }
        else if (!strcmp(argv[i], "--set-host") && i + 1 < argc) set_host_to = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc)     set_host_user = argv[++i];
        else if (!strcmp(argv[i], "--fal-selftest")) fal_self_test = 1;
        else if (!strcmp(argv[i], "--fal-accept-test")) fal_accept_test = 1;
        else if (!strcmp(argv[i], "--router")) router_mode = 1;
        else if (!strcmp(argv[i], "--priority") && i + 1 < argc)
            router_priority = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "DECNETD-E-BADARG, unknown argument '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* --router --self-test: the router run-mode isolation proof (rd vms-0a9),
     * no netdev, NO real node. Dispatched before the plain --self-test. */
    if (router_mode && self_test)
        return run_router_self_test();
    if (self_test)
        return run_self_test();
    if (nsp_self_test)
        return run_nsp_selftest();
    if (sethost_self_test)
        return run_sethost_selftest();
    if (sethost_srccode_test)
        return run_sethost_srccode_selftest();
    if (cterm_accept_test)
        return run_cterm_accept_test();
    if (isolation_test)
        return run_isolation_test();
    if (fal_self_test)
        return run_fal_selftest();
    if (fal_accept_test)
        return run_fal_accept_test();

    /* A router routes and a --set-host CLIENT bridges a terminal; neither is a
     * NETACP that serves inbound object-42 sessions unless the caller pins it on.
     * The persistent ENDNODE daemon serves by default (see cterm_server above). */
    if ((router_mode || set_host_to) && !cterm_server_explicit)
        cterm_server = 0;

    /* SELF-SOURCE the executor address from the node's DECnet configuration
     * (executor.dat, rd vms-f54) whenever --address was not given -- for the
     * --set-host CLIENT (so DCL's SET HOST wiring need not know it), for the
     * persistent NETACP daemon SYS$MANAGER:STARTNET.COM starts with no argv
     * (rd vms-a70 direction B), and for --show-executor. If executor.dat is
     * absent the NOADDRESS error below fires -- DECnet is simply not configured
     * on this node, and no address is ever invented (INV-6). */
    static char sethost_addrbuf[16];
    if (!addr_s) {
        unsigned ea = 0, en = 0;
        if (sethost_source_executor(&ea, &en) == 0) {
            snprintf(sethost_addrbuf, sizeof(sethost_addrbuf), "%u.%u", ea, en);
            addr_s = sethost_addrbuf;
        }
    }

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

    /* --priority is only meaningful in --router mode, and is a single wire byte. */
    if (router_priority < 0 || router_priority > 255) {
        fprintf(stderr, "DECNETD-E-BADPRIO, --priority must be 0..255 (DR-election"
                        " priority); got %d\n", router_priority);
        return 1;
    }
    if (router_priority && !router_mode) {
        fprintf(stderr, "DECNETD-E-BADPRIO, --priority requires --router\n");
        return 1;
    }

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
        if (router_mode)
            dnet_engine_set_router(&e, (uint8_t)router_priority);
        dnet_engine_show_executor(&e, stdout);
        dnet_engine_show_circuit(&e, stdout);
        /* Report, honestly, whether the persistent daemon would SERVE inbound
         * $ SET HOST -- the serve decision STARTNET.COM's NETACP inherits (the
         * endnode daemon serves object 42 by default; a router or --set-host
         * client, or --no-cterm-server, does not). This is a dry-run readout:
         * --show-executor opens no socket, so it never actually serves here. */
        printf("Inbound SET HOST (object 42) = %s\n",
               cterm_server ? "served (CTERM -> LOGINOUT)" : "not served");
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
    /* --router (rd vms-0a9): advertise an L1 router node-type and emit router-
     * hellos on the T3 cadence instead of endnode-hellos, so a Phase IV endnode
     * on the segment selects THIS node as its designated router. */
    if (router_mode)
        dnet_engine_set_router(&eng, (uint8_t)router_priority);

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
    printf(" DECNETD-I-STARTED, NETACP up: DECnet Phase IV %s on circuit %s"
           " (wire engine demoted to NETACP's datalink; AF_PACKET hidden behind"
           " the _NET: device face, Rule 1)\n",
           router_mode ? "L1 router" : "endnode", eng.circuit);
    dnet_engine_show_executor(&eng, stdout);
    dnet_engine_show_circuit(&eng, stdout);
    /* Say, honestly, whether this NETACP serves inbound $ SET HOST. When it
     * does, an inbound object-42 connect reaches LOGINOUT on an executive-minted
     * RTAn: (one session at a time); the remote user authenticates fresh. */
    log_ts(stdout);
    if (cterm_server)
        printf(" DECNETD-I-CTERMLISTEN, serving inbound $ SET HOST (Session"
               " Control object 42 -> LOGINOUT on RTAn:); one session at a time\n");
    else
        printf(" DECNETD-I-ROUTEONLY, NOT serving inbound $ SET HOST"
               " (routing only)\n");
    fflush(stdout);

    /* --set-host CLIENT (rd vms-f54): the OUTBOUND half of $ SET HOST. It opens
     * a CTERM terminal session to object 42 on the remote node and bridges THIS
     * process's VMS terminal channel to it, then returns -- it owns its own loop
     * and never falls through to the routing/inbound loop below. */
    if (set_host_to) {
        unsigned ta = 0, tn = 0;
        if (sethost_resolve_target(set_host_to, &ta, &tn) != 0) {
            fprintf(stderr, "DECNETD-E-NOSUCHNODE, --set-host: cannot resolve"
                            " node '%s' (not area.node, and not a NAME in the"
                            " node database)\n", set_host_to);
            scs_datalink_close(sock);
            return 1;
        }
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%u.%u", ta, tn);
        int r = run_set_host_loop(&eng, sock, ifindex, tbuf, set_host_user);
        scs_datalink_close(sock);
        return r;
    }

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

        /* T3 emission cadence: build + transmit our HELLO. In --router mode this
         * is a spec-faithful ROUTER-hello to the all-endnodes multicast (so an
         * endnode selects us as its DR); otherwise the endnode-hello (rd vms-0a9). */
        if (dnet_engine_hello_due(&eng, now)) {
            size_t flen = 0;
            int is_router = dnet_engine_is_router(&eng);
            int built = is_router
                ? dnet_engine_build_router_hello_frame(&eng, frame, sizeof(frame), &flen)
                : dnet_engine_build_hello_frame(&eng, frame, sizeof(frame), &flen);
            if (built == DNET_ENGINE_OK) {
                const uint8_t *mcast = is_router ? DNET_ROUTER_HELLO_MCAST
                                                 : DNET_HELLO_MCAST;
                ssize_t sent = scs_datalink_send(sock, (int)ifindex,
                                                 DNET_ETHERTYPE, mcast,
                                                 frame, flen);
                if (sent < 0) {
                    fprintf(stderr, "DECNETD-E-SENDFAIL, HELLO transmit failed: %s\n",
                            strerror(errno));
                } else {
                    if (is_router)
                        dnet_engine_router_hello_emitted(&eng, now);
                    else
                        dnet_engine_hello_emitted(&eng, now);
                    log_ts(stdout);
                    printf(" DECNETD-I-HELLOSENT, %s circuit %s seq=%lu bytes=%zd\n",
                           is_router ? "router-hello" : "endnode-hello",
                           eng.circuit,
                           is_router ? eng.router_hello_sent : eng.hello_sent, sent);
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
