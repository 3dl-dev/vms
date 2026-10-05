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
#include <fcntl.h>      /* open(/dev/urandom): the NETACP link-handle secret */
#include <net/if.h>      /* if_nametoindex() */
#include <ifaddrs.h>    /* getifaddrs(): auto-detect the primary NIC (no argv) */
#include <poll.h>       /* the --cterm-server loop waits on wire + session */
#include <pthread.h>    /* --fal-accept-test / --fal-selftest: two blocking peers */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    /* strcasecmp for --set-host node-name resolution */
#include <sys/socket.h>
#include <sys/time.h>   /* struct timeval for the datalink SO_RCVTIMEO (--copy) */
#include <time.h>
#include <unistd.h>

#include "dnet_engine.h"
#include "dnet_cterm.h"     /* CTERM terminal-service protocol (--set-host-selftest) */
#include "dnet_cterm_host.h" /* CTERM HOST session: $CREPRC -> LOGINOUT on RTAn: */
#include "dnet_cterm_hostfsm.h" /* CTERM HOST wire role, as a VMS host speaks it */
#include "dnet_dap.h"       /* DAP message codec (--fal-* : COPY presentation layer) */
#include "dnet_fal.h"
#include "dnet_fal_proc.h"  /* FAL network server process (rd vms-d85) */
#include "dnet_mail11.h"    /* MAIL-11 receiver (object 27, rd vms-47fd)    */
#include "dnet_mail_proc.h" /* MAIL_SERVER.EXE network server process      */       /* FAL server + COPY client (object 17, rd vms-8c2) */
#include "dnet_broker.h"    /* exec<->NETACP T1 broker record codec (rd vms-22c) */
#include "dnet_netqio.h"    /* a _NET: $QIOW ends with its IOSB status (rd vms-d01) */
#include "dnet_netshow.h"    /* NCP SHOW snapshot record NETACP answers (rd vms-30e) */
#include "ovmx_status.h"     /* vms_status_string: the authentic %FAC-S-ID text */
#include "dnet_ncb.h"       /* NCB connect-block parser ($QIO IO$_ACCESS, vms-22c) */
#include "dnet_nodespec.h"  /* node-filespec splitter + COPY plan (rd vms-ea8) */
#include "dnet_ncpstore.h"  /* SYS$SYSTEM:NETNODE_*.DAT via RMS (rd vms-1f69) */
#include "rms_textfile.h"   /* --fal-accept-test byte-verify: RMS over the ACP */
#include "rms_io.h"         /* --fal-proc-accept-test: a fixture with an explicit protection */
#include "sysuaf.h"         /* the recipient's UIC: mail-file owner check */
#include "rms/rms.h"        /* sys$erase / rms_file_attr: the MAIL-11 test leaves no node DB */
#include "vmsfs/ods2.h"     /* ODS2_FK_DATA_STMLF */
#include "ovmx_identity.h"  /* INV-1 identity SSOT: human banner = OVMX product id */
#include "scs_datalink.h"   /* the shared raw-L2 datalink (src/libdatalink) */
#include "ssdef.h"          /* SS$_BADPARAM (isolation-seam refusal, vms-9ab) */
#include "rmsdef.h"         /* RMS$_FNF: --fal-proc-accept-test DELETE readback (vms-277a) */
#include "vms_kif.h"        /* $GETDVI readback: the sec-7.5 anti-LARP tell */
#include "prvdef.h"         /* PRV$M_NETMBX: a broker requester must hold it (rd vms-c6d1) */
#include "prcdef.h"         /* PRC$M_DETACH: the vms-c6d1 request-mailbox probe process */
#include "vmsfs/filespec.h" /* vmsfs_to_linux_path: SYS$SYSTEM:DECNETD.EXE for that probe */
#include "ovmx_layout.h"    /* ovmx_boot_stage_exec_path                              */
#include "starlet.h"        /* vms-f54 CLIENT: $ASSIGN/$QIO(W)/$DASSGN terminal I/O */
#include "descrip.h"        /* dsc$descriptor_s for the SYS$INPUT/SYS$OUTPUT assign */
#include "iodef.h"          /* IO$_READVBLK/WRITEVBLK/SETMODE + IO$K_TT_PASSALL      */
#include "lnmdef.h"         /* LNM$C_USER: the DNET$NETACP_REQ logical (rd vms-dda)   */

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

/*
 * decnet_autodetect_iface - the datalink interface the persistent daemon binds
 * when SYS$MANAGER:STARTNET.COM starts it with no argv (rd vms-a70 direction B).
 *
 * VMS RUN passes an image no argv, so the detached NETACP cannot be told
 * --iface; and the compile-time DECNETD_DEFAULT_IFACE ("br0") is a DEV-LAB
 * bridge name that does not exist inside a booted node's own network namespace
 * (there the primary NIC is eth0/ETH0:). So when no --iface is given, pick the
 * FIRST up, non-loopback interface that has a link-layer (Ethernet) address --
 * the primary NIC, the same one the executive's DECnet device face _NET: rides
 * (src/kernel-core/vms_devtab.c vms_devtab_probe_net). Returns 1 and fills buf
 * on success, 0 if nothing suitable was found (caller keeps the compiled
 * default). Multi-NIC circuit selection (NCP SET EXECUTOR/CIRCUIT to a specific
 * line) is a follow-on; a single-NIC node -- the booted-OVMX case -- resolves
 * unambiguously here. Pure enumeration: opens no socket, needs no privilege.
 */
static int decnet_autodetect_iface(char *buf, size_t sz)
{
    /* rd vms-1f69: the FIRST non-loopback Ethernet netdev, via the shared
     * userspace twin of the executive's exec_netdev_primary()
     * (scs_datalink_primary_iface) -- the SAME classification that names
     * ETH0:/_NET:. It deliberately does NOT require IFF_UP: on a booted node
     * nothing brings eth0 up before NETACP starts (the executive's L2_OPEN does,
     * exec_netdev_ensure_up), so an up-only scan found nothing and fell back to
     * the dev-lab "br0" -- observed: --show-executor reported br0 on a booted
     * guest whose NIC is eth0, and the datalink then failed SS$_NOSUCHDEV. */
    if (!buf || sz == 0)
        return 0;
    return scs_datalink_primary_iface(buf, sz) == 0 && buf[0] != '\0';
}

/* A monotonic seconds tick -- the unit the engine's T3/listen timers use. */
static dnet_tick_t monotonic_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (dnet_tick_t)ts.tv_sec;
}

/* A monotonic millisecond clock: the CTERM host's output-quiet timer. */
static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
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
 * DECnet configuration self-sourcing (rd vms-f54; storage rd vms-1f69).
 *
 * The daemon reads its OWN executor address and resolves a target NODE NAME
 * from the node's DECnet configuration databases -- the SAME databases NCP
 * writes, through the SAME store (src/vmsdecnet/ncp/dnet_ncpstore.c):
 * SYS$SYSTEM:NETNODE_LOCAL.DAT (executor) and SYS$SYSTEM:NETNODE_REMOTE.DAT
 * (nodes), reached through the VMS file layer (RMS over the Files-11 ACP), so
 * there is ONE config SSOT, never a second ledger, and no Linux-path default.
 * (OVMX_DECNET_EXECUTOR / _NODEDB remain the host-test hook.) Missing or
 * unconfigured -> honest failure, never a guess (INV-6).
 */

/* Read the local executor address. Returns 0 and fills area/node on success. */
static int sethost_source_executor(unsigned *area, unsigned *node)
{
    struct dnet_executor x;
    if (dnet_store_load_executor(&x) != DNET_STORE_OK || !x.have_addr)
        return -1;
    *area = dnet_area_of(x.addr);
    *node = dnet_node_of(x.addr);
    return 0;
}

/* Resolve a --set-host target: accept "area.node" directly, else look the token
 * up as a NODE NAME (case-insensitive) in the remote node database. Returns 0
 * and fills area/node. */
static int sethost_resolve_target(const char *token, unsigned *area, unsigned *node)
{
    if (parse_addr(token, area, node) == 0)
        return 0;
    static struct dnet_nodedb db;
    if (dnet_store_load_nodes(&db) != DNET_STORE_OK)
        return -1;
    const struct dnet_node_entry *e = dnet_nodedb_by_name(&db, token);
    if (!e)
        return -1;
    *area = dnet_area_of(e->addr);
    *node = dnet_node_of(e->addr);
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
 * run_net_ncb_selftest (rd vms-22c, a1-2) -- the host floor of the DECnet
 * Network Connect Block parser (dnet_ncb_parse): the LOCAL API connect string a
 * task-to-task application hands $QIO IO$_ACCESS on a _NET: channel, which
 * qio_net_op parses into {node, task/object} before building the (oracle-
 * grounded) SC connect descriptor. Proves the documented connect-string forms
 * parse and every malformed NCB is refused bounds-safe (never over-read) -- no
 * executive needed (the NCB never touches the wire; it cannot crash a peer).
 */
static int run_net_ncb_selftest(void)
{
    printf("DECNETD-I-NETNCB, NCB connect-block parser (NODE::\"TASK=name\" ->"
           " {node, task/object}) + bounds-safe refusal of malformed input (no"
           " executive, spec-derived local API, rd vms-22c)\n");
    int pass = 0, fail = 0;
#define NC_CHECK(c, msg) do { if (c) { pass++; printf("  PASS: %s\n", msg); } \
    else { fail++; printf("  FAIL: %s\n", msg); } } while (0)

    struct dnet_ncb n;
    /* documented forms (strlen avoids hand-counted length bugs) */
#define NCB_S(str) (str), strlen(str), &n
    NC_CHECK(dnet_ncb_parse(NCB_S("OVMXR::\"TASK=SERVER\"")) == DNET_NCB_OK &&
             !strcmp(n.node, "OVMXR") && n.is_named && !strcmp(n.task, "SERVER"),
             "NODE::\"TASK=name\" -> node + named task");
    NC_CHECK(dnet_ncb_parse(NCB_S("1.11::\"TASK=ECHO\"")) == DNET_NCB_OK &&
             !strcmp(n.node, "1.11") && n.is_named && !strcmp(n.task, "ECHO"),
             "area.node literal target parses (1.11::\"TASK=ECHO\")");
    NC_CHECK(dnet_ncb_parse(NCB_S("NODE::\"0=SVC\"")) == DNET_NCB_OK &&
             !strcmp(n.node, "NODE") && n.is_named && !strcmp(n.task, "SVC"),
             "NODE::\"0=name\" (object 0 named task) parses");
    NC_CHECK(dnet_ncb_parse(NCB_S("NODE::\"17\"")) == DNET_NCB_OK &&
             !strcmp(n.node, "NODE") && !n.is_named && n.object == 17,
             "NODE::\"17\" -> a well-known object NUMBER");
    NC_CHECK(dnet_ncb_parse(NCB_S("NODE::\"BARE\"")) == DNET_NCB_OK &&
             n.is_named && !strcmp(n.task, "BARE"),
             "NODE::\"bare\" -> a named object");
    NC_CHECK(dnet_ncb_parse(NCB_S("NODE::SERVER")) == DNET_NCB_OK &&
             !strcmp(n.node, "NODE") && n.is_named && !strcmp(n.task, "SERVER"),
             "an unquoted object spec (NODE::SERVER) parses");
    /* rd vms-dda: access control + the documented "17=" object form */
    NC_CHECK(dnet_ncb_parse(NCB_S("VAX1\"SYSTEM MANAGER\"::\"17=\"")) == DNET_NCB_OK &&
             !strcmp(n.node, "VAX1") && n.has_access && !strcmp(n.user, "SYSTEM") &&
             !strcmp(n.password, "MANAGER") && n.account[0] == '\0' &&
             !n.is_named && n.object == 17,
             "NODE\"user password\"::\"17=\" -> node + access control + object 17");
    NC_CHECK(dnet_ncb_parse(NCB_S("1.11\"U P:W ACCT\"::\"0=ECHO/data\"")) == DNET_NCB_OK &&
             !strcmp(n.node, "1.11") && !strcmp(n.password, "P:W") &&
             !strcmp(n.account, "ACCT") && n.is_named && !strcmp(n.task, "ECHO"),
             "a password holding ':' and an account parse; a /connect-data tail is ignored");
    NC_CHECK(dnet_ncb_parse(NCB_S("N\"A B C D\"::\"17=\"")) == DNET_NCB_EBADLEN &&
             n.node[0] == '\0' && n.password[0] == '\0',
             "a four-field access string is refused and nothing (no password) is left behind");
    NC_CHECK(dnet_ncb_parse(NCB_S("N\"\"::\"17=\"")) == DNET_NCB_ETRUNC,
             "an empty access string is refused");
    NC_CHECK(dnet_ncb_parse(NCB_S("N\"USER::\"17=\"")) == DNET_NCB_ETRUNC,
             "an unterminated access-control quote is refused (no \"::\" outside quotes)");

    /* bounds-safe refusals -- never over-read */
    NC_CHECK(dnet_ncb_parse("", 0, &n) == DNET_NCB_ETRUNC,
             "empty NCB is refused (no \"::\")");
    NC_CHECK(dnet_ncb_parse("NODE", 4, &n) == DNET_NCB_ETRUNC,
             "a spec with no \"::\" is refused");
    NC_CHECK(dnet_ncb_parse("::\"X\"", 5, &n) == DNET_NCB_ETRUNC,
             "an empty node is refused");
    NC_CHECK(dnet_ncb_parse("NODE::", 6, &n) == DNET_NCB_ETRUNC,
             "an empty object spec is refused");
    NC_CHECK(dnet_ncb_parse("NODE::\"\"", 8, &n) == DNET_NCB_ETRUNC,
             "an empty quoted object spec is refused");
    NC_CHECK(dnet_ncb_parse(NCB_S("THISNODENAMEISWAYTOOLONG::\"X\"")) == DNET_NCB_EBADLEN,
             "an over-long node name is refused (EBADLEN, not truncated)");
    NC_CHECK(dnet_ncb_parse(NCB_S("N::\"TASK=THISTASKNAMEISWAYTOOLONG\"")) == DNET_NCB_EBADLEN,
             "an over-long task name is refused (EBADLEN)");
    NC_CHECK(dnet_ncb_parse(NULL, 5, &n) == DNET_NCB_EINVAL,
             "a null NCB is refused (EINVAL)");
#undef NCB_S

    /* fuzz: mutate a valid NCB + feed every truncated prefix; must never crash
     * (ASan/UBSan) and never accept a self-inconsistent parse. */
    {
        const char *base = "OVMXR::\"TASK=SERVER\"";
        size_t blen = strlen(base);
        uint32_t seed = 0x1234abcdu;
        int inconsistent = 0, refused = 0, accepted = 0;
        for (int i = 0; i < 200000; i++) {
            char fz[40];
            memcpy(fz, base, blen);
            for (int m = 0; m < 3; m++) {
                seed = seed * 1664525u + 1013904223u;
                fz[(seed >> 8) % blen] = (char)(seed & 0xff);
            }
            seed = seed * 1664525u + 1013904223u;
            size_t use = (seed >> 8) % (blen + 1);   /* truncated prefixes too */
            struct dnet_ncb fn;
            int r = dnet_ncb_parse(fz, use, &fn);
            if (r == DNET_NCB_OK) {
                accepted = 1;
                /* an accepted named parse must have a NUL-terminated bounded task */
                if (fn.is_named && strnlen(fn.task, sizeof fn.task) >= sizeof fn.task)
                    inconsistent = 1;
                if (strnlen(fn.node, sizeof fn.node) >= sizeof fn.node)
                    inconsistent = 1;
            } else {
                refused = 1;
            }
        }
        NC_CHECK(!inconsistent && refused,
                 "200k-mutation + truncated-prefix fuzz: no accepted parse is bounds-inconsistent, malformed refused (ASan/UBSan clean)");
        (void)accepted;
    }

    printf("DECNETD-I-NETNCB, %d passed, %d failed\n", pass, fail);
    if (fail == 0 && pass > 0) { printf("DECNETD-NET-NCB-SELFTEST: PASS\n"); return 0; }
    printf("DECNETD-NET-NCB-SELFTEST: FAIL\n");
    return 1;
#undef NC_CHECK
}

/*
 * run_net_broker_selftest (rd vms-22c, a1-2) -- the host floor of the exec<->
 * NETACP T1 broker transport: the request/response RECORD CODEC that rides the
 * executive mailbox, and its two SECURITY GUARDS, proven with NO executive and
 * NO mailbox (pure codec logic, the discipline of --nsp-selftest / the CTERM
 * codec fuzz). It proves:
 *   (A) a request and a response round-trip byte-exact (encode -> decode);
 *   (B) BOUNDS VALIDATION -- a truncated header, a truncated body, an over-bound
 *       datalen, and a wrong/opposite magic are each REFUSED, never over-read
 *       (the a1-2 seam that keeps a malformed mailbox record from faulting the
 *       executive or NETACP); a 200k-iteration mutation + every-truncated-prefix
 *       fuzz decodes clean (ASan/UBSan) and never returns a self-inconsistent
 *       record;
 *   (C) CORRELATION IDS -- monotonic nonzero issuance, and the anti-cross-talk
 *       match: a response is accepted for a request ONLY when the ids are equal
 *       and nonzero, so a reply meant for another link is refused.
 * The mailbox seam + qio_net_op marshalling + NETACP servicing (the /dev/vms
 * rungs) build on this proven record.
 */
static uint32_t nbself_rand(uint32_t *s)   /* deterministic LCG for the fuzz */
{
    *s = (*s) * 1664525u + 1013904223u;
    return *s;
}

static int run_net_broker_selftest(void)
{
    printf("DECNETD-I-NETBROKER, exec<->NETACP T1 broker record codec: round-trip"
           " + bounds-validated decode + correlation-id anti-cross-talk (no"
           " executive, rd vms-22c)\n");
    int pass = 0, fail = 0;
#define NB_CHECK(c, msg) do { if (c) { pass++; printf("  PASS: %s\n", msg); } \
    else { fail++; printf("  FAIL: %s\n", msg); } } while (0)

    /* (A) request round-trip byte-exact. */
    struct dnet_broker_req req, rq2;
    memset(&req, 0, sizeof req);
    req.corr_id = 0x11223344u; req.owner_pid = 0x0000BEEFu;
    req.link_handle = 0x2001u; req.reply_unit = 0x00000042u;
    req.op = DNET_BROKER_OP_SEND;
    const char *payload = "TASK-TO-TASK broker payload: ping 0123456789";
    req.datalen = (uint16_t)strlen(payload);
    memcpy(req.data, payload, req.datalen);

    uint8_t buf[DNET_BROKER_REQ_MAX + 8];
    size_t blen = 0;
    int enc = dnet_broker_req_encode(&req, buf, sizeof buf, &blen);
    int dec = dnet_broker_req_decode(buf, blen, &rq2);
    NB_CHECK(enc == DNET_BROKER_OK && dec == DNET_BROKER_OK &&
             blen == (size_t)DNET_BROKER_REQ_HDR + req.datalen &&
             rq2.corr_id == req.corr_id && rq2.owner_pid == req.owner_pid &&
             rq2.link_handle == req.link_handle && rq2.reply_unit == req.reply_unit &&
             rq2.op == req.op &&
             rq2.datalen == req.datalen &&
             memcmp(rq2.data, req.data, req.datalen) == 0,
             "request record round-trips byte-exact (encode -> decode)");

    /* response round-trip byte-exact. */
    struct dnet_broker_rsp rsp, rs2;
    memset(&rsp, 0, sizeof rsp);
    rsp.corr_id = req.corr_id; rsp.status = 0x00000001u /* SS$_NORMAL */;
    const char *rdata = "TASK-TO-TASK broker reply: pong 9876543210";
    rsp.datalen = (uint16_t)strlen(rdata);
    memcpy(rsp.data, rdata, rsp.datalen);
    uint8_t rbuf[DNET_BROKER_RSP_MAX + 8];
    size_t rlen = 0;
    NB_CHECK(dnet_broker_rsp_encode(&rsp, rbuf, sizeof rbuf, &rlen) == DNET_BROKER_OK &&
             dnet_broker_rsp_decode(rbuf, rlen, &rs2) == DNET_BROKER_OK &&
             rs2.corr_id == rsp.corr_id && rs2.status == rsp.status &&
             rs2.datalen == rsp.datalen &&
             memcmp(rs2.data, rsp.data, rsp.datalen) == 0,
             "response record round-trips byte-exact (encode -> decode)");

    /* (B) BOUNDS VALIDATION -- every malformed record is refused, never over-read. */
    NB_CHECK(dnet_broker_req_decode(buf, DNET_BROKER_REQ_HDR - 1, &rq2) == DNET_BROKER_ETRUNC,
             "a header-truncated record is refused (ETRUNC), not misread");
    NB_CHECK(dnet_broker_req_decode(buf, blen - 1, &rq2) == DNET_BROKER_ETRUNC,
             "a body-truncated record is refused (ETRUNC), never over-reads the payload");
    {
        /* forge a header claiming datalen over the payload bound. */
        uint8_t bad[DNET_BROKER_REQ_HDR];
        memcpy(bad, buf, DNET_BROKER_REQ_HDR);
        bad[22] = (uint8_t)((DNET_NSP_MAX_DATA + 1) & 0xff);   /* datalen field (hdr grew to 24) */
        bad[23] = (uint8_t)(((DNET_NSP_MAX_DATA + 1) >> 8) & 0xff);
        NB_CHECK(dnet_broker_req_decode(bad, sizeof bad, &rq2) == DNET_BROKER_EBADLEN,
                 "a datalen over DNET_NSP_MAX_DATA is refused (EBADLEN), never allocates/reads it");
    }
    NB_CHECK(dnet_broker_rsp_decode(buf, blen, &rs2) == DNET_BROKER_EMAGIC,
             "a REQUEST decoded as a RESPONSE is refused by the magic gate (direction guard)");
    {
        uint8_t bad[DNET_BROKER_REQ_HDR];
        memcpy(bad, buf, DNET_BROKER_REQ_HDR);
        bad[0] ^= 0xff;   /* corrupt the magic */
        NB_CHECK(dnet_broker_req_decode(bad, sizeof bad, &rq2) == DNET_BROKER_EMAGIC,
                 "a wrong-magic record is refused (EMAGIC), not misread as a request");
    }

    /* fuzz: mutate a valid record + feed every truncated prefix; must never
     * crash (ASan/UBSan) and never return OK with a self-inconsistent length. */
    {
        uint32_t seed = 0xC0FFEEu;
        int ok_seen = 0, refused_seen = 0, inconsistent = 0;
        for (int i = 0; i < 200000; i++) {
            uint8_t fz[DNET_BROKER_REQ_MAX + 8];
            size_t n = blen ? blen : 1;
            memcpy(fz, buf, n);
            /* mutate a few bytes */
            for (int m = 0; m < 3; m++)
                fz[nbself_rand(&seed) % n] = (uint8_t)nbself_rand(&seed);
            size_t use = (nbself_rand(&seed) % (n + 1));   /* every truncated prefix too */
            struct dnet_broker_req fr;
            int r = dnet_broker_req_decode(fz, use, &fr);
            if (r == DNET_BROKER_OK) {
                ok_seen = 1;
                if ((size_t)DNET_BROKER_REQ_HDR + fr.datalen > use ||
                    fr.datalen > DNET_NSP_MAX_DATA)
                    inconsistent = 1;   /* an accepted record must be length-consistent */
            } else {
                refused_seen = 1;
            }
        }
        NB_CHECK(!inconsistent && refused_seen,
                 "200k-mutation + truncated-prefix fuzz: no accepted record is length-inconsistent, and malformed inputs are refused (ASan/UBSan clean)");
        (void)ok_seen;
    }

    /* (C) CORRELATION IDS -- monotonic issuance + the anti-cross-talk match. */
    {
        uint32_t st = 0, a, b, c;
        a = dnet_broker_corr_next(&st);
        b = dnet_broker_corr_next(&st);
        c = dnet_broker_corr_next(&st);
        NB_CHECK(a != 0 && b == a + 1 && c == b + 1,
                 "correlation ids are issued monotonically, skipping the 0 sentinel");
        NB_CHECK(dnet_broker_corr_match(a, a) == 1 &&
                 dnet_broker_corr_match(a, b) == 0 &&
                 dnet_broker_corr_match(0, 0) == 0 &&
                 dnet_broker_corr_match(a, 0) == 0,
                 "corr_match accepts an exact nonzero match and refuses a mismatch / the 0 sentinel");
    }
    /* the anti-cross-talk scenario over the real records: a response carrying
     * another link's correlation id is refused delivery to this request. Decode
     * fresh records here (rq2/rs2 above were reused as scratch by the bounds
     * checks, whose refusals zero *out). */
    {
        struct dnet_broker_req freq;
        struct dnet_broker_rsp frsp;
        NB_CHECK(dnet_broker_req_decode(buf, blen, &freq) == DNET_BROKER_OK &&
                 dnet_broker_rsp_decode(rbuf, rlen, &frsp) == DNET_BROKER_OK &&
                 dnet_broker_corr_match(freq.corr_id, frsp.corr_id) == 1 &&
                 dnet_broker_corr_match(freq.corr_id, frsp.corr_id ^ 0x1u) == 0,
                 "a response is delivered to its request only on an exact corr-id match (cross-talk refused)");
    }

    printf("DECNETD-I-NETBROKER, %d passed, %d failed\n", pass, fail);
    if (fail == 0 && pass > 0) { printf("DECNETD-NET-BROKER-SELFTEST: PASS\n"); return 0; }
    printf("DECNETD-NET-BROKER-SELFTEST: FAIL\n");
    return 1;
#undef NB_CHECK
}

/*
 * broker_open_connect (rd vms-dda) -- turn a broker OPEN's payload, the NCB text
 * the client handed $QIO IO$_ACCESS (NODE"user password account"::"object"), into
 * a resolved target + the Session Control connect NETACP sends. This is NETACP's
 * job on VMS, not the client's:
 *   - the node is resolved from NETACP's own node database ("area.node"
 *     literals too; "0" and the executor's own name are THIS node, reached over
 *     the local loopback);
 *   - the connect's SOURCE descriptor is the REQUESTING process's identity --
 *     its UIC group/member and username, read from the executive by pid
 *     ($GETJPI), never taken from the request; a pid the executive does not
 *     know is refused. With no executive (the host floor) NETACP falls back to
 *     its own nonzero identity (sethost_src_codes) and its node name, LABELLED.
 * Bounds: the NCB parse is the bounds-checked dnet_ncb_parse; every refusal is
 * an honest SS$_ status with nothing sent. The parsed password is wiped.
 */
static uint32_t broker_open_connect(const struct dnet_engine *node, uint32_t owner_pid,
                                    const uint8_t *ncbtxt, size_t ncblen,
                                    unsigned *rarea, unsigned *rnode, int *object,
                                    uint8_t *desc, size_t cap, size_t *dlen)
{
    struct dnet_ncb n;
    uint32_t st = SS$_NORMAL;
    if (dnet_ncb_parse((const char *)ncbtxt, ncblen, &n) != DNET_NCB_OK)
        return SS$_BADPARAM;

    if (strcmp(n.node, "0") == 0 || strcasecmp(n.node, node->node_name) == 0) {
        *rarea = dnet_area_of(node->addr);
        *rnode = dnet_node_of(node->addr);
    } else if (sethost_resolve_target(n.node, rarea, rnode) != 0) {
        st = SS$_NOSUCHDEV;               /* not area.node, not in the node DB */
        goto out;
    }

    uint16_t grp = 0, usr = 0;
    char srcuser[DNET_SC_MAX_STR + 1] = "";
    if (owner_pid != 0) {
        struct vms_procinfo pi;
        uint32_t js = vms_kif_getjpi_pid(owner_pid, &pi);
        if (js == SS$_NONEXPR) {
            st = SS$_BADPARAM;            /* no such requesting process */
            goto out;
        }
        if (js & 1) {
            grp = (uint16_t)(pi.uic >> 16);
            usr = (uint16_t)(pi.uic & 0xFFFF);
            size_t k = 0;
            while (k < sizeof pi.username && k < DNET_SC_MAX_STR &&
                   pi.username[k] && pi.username[k] != ' ') {
                srcuser[k] = pi.username[k];
                k++;
            }
            srcuser[k] = '\0';
        }
    }
    if (grp == 0 && usr == 0)
        sethost_src_codes(&grp, &usr);
    if (!srcuser[0])
        snprintf(srcuser, sizeof srcuser, "%s", node->node_name);

    int rc;
    if (n.is_named) {
        *object = 0;
        rc = dnet_cterm_sc_connect_build_task(n.task, srcuser, grp, usr, n.user,
                                              n.password, n.account, desc, cap, dlen);
    } else if (n.object > 255) {
        st = SS$_BADPARAM;
        goto out;
    } else {
        *object = (int)n.object;
        rc = dnet_cterm_sc_connect_build((uint8_t)n.object, srcuser, grp, usr, n.user,
                                         n.password, n.account, desc, cap, dlen);
    }
    if (rc != DNET_CTERM_OK)
        st = SS$_BADPARAM;
out:
    memset(&n, 0, sizeof n);              /* the parsed access-control password */
    return st;
}

/*
 * dnet_broker_serve (rd vms-22c, a1-2 integration) -- service ONE bounds-
 * validated broker request against the NSP link engine, producing a
 * correlation-matched response. This is the NETACP "brain" the mailbox serve
 * loop will call: the caller has already dnet_broker_req_decode'd the request
 * (so every field is length-checked) and owns the datalink; this maps the broker
 * OP to the engine's link primitive and, when the op emits a wire frame (CI /
 * data segment / DI), returns it in frame_out for the caller to transmit.
 *
 * NEVER-CRASH-A-PEER (INV-6 / the A2/A8 discipline): an unknown op, an op invalid
 * for the current link state, or an OPEN whose payload is too short for the
 * remote address are each answered with an HONEST error status and no frame --
 * never a crash, an over-read, or a fabricated success. rsp->corr_id ALWAYS
 * echoes req->corr_id so the waiter matches the completion (dnet_broker_corr_
 * match); a response is never emitted with a zero/mismatched id.
 *
 * Request payload layout (data[], already length-bounded by the decode):
 *   OP_OPEN : the NCB text the client gave IO$_ACCESS; NETACP resolves the node
 *             from the node database it owns and builds the Session Control
 *             connect (broker_open_connect, rd vms-dda).
 *   OP_SEND : the raw task-to-task message bytes.
 *   OP_RECV / OP_CLOSE : no request payload.
 * OP_OPEN sends the Connect Initiate and returns SS$_NORMAL "initiated" -- the
 * link reaches RUN asynchronously when the peer's Connect Confirm arrives (the
 * mailbox serve loop delivers that completion later); OP_RECV returns the next
 * buffered inbound segment or SS$_ENDOFFILE when nothing is pending (the loop
 * owns the blocking/wait semantics, this call never blocks).
 */
static int dnet_broker_serve(struct dnet_engine *eng,
                             const struct dnet_broker_req *req,
                             struct dnet_broker_rsp *rsp,
                             uint8_t *frame_out, size_t framecap,
                             size_t *framelen, int *has_frame, dnet_tick_t now)
{
    if (!eng || !req || !rsp || !frame_out || !framelen || !has_frame)
        return -1;

    memset(rsp, 0, sizeof *rsp);
    rsp->corr_id = req->corr_id;          /* always echo -- the correlation guard */
    *has_frame = 0;
    *framelen  = 0;

    size_t flen = 0;

    switch (req->op) {
    case DNET_BROKER_OP_OPEN:
        /* data = the NCB text (NODE"user password account"::"object"), the
         * single OPEN contract NETACP's pool also serves (rd vms-dda). NETACP
         * resolves the node and builds the connect (broker_open_connect);
         * a malformed NCB or an unresolvable node is refused honestly, never
         * over-read or faked (INV-6). */
        {
            unsigned rarea = 0, rnode = 0; int obj = 0;
            uint8_t desc[192]; size_t desclen = 0;
            uint32_t ost = broker_open_connect(eng, req->owner_pid, req->data, req->datalen,
                                               &rarea, &rnode, &obj, desc, sizeof desc,
                                               &desclen);
            if (!(ost & 1)) {
                rsp->status = ost;
                return 0;
            }
            int orc = dnet_engine_link_open(eng, rarea, rnode, 0x2001, desc, desclen,
                                            1459, 1, DNET_NSP_VER_41,
                                            frame_out, framecap, &flen, now);
            memset(desc, 0, sizeof desc);
            if (orc != DNET_ENGINE_OK) {
                rsp->status = SS$_ABORT;
                return 0;
            }
            *framelen = flen; *has_frame = 1;
            rsp->status = SS$_NORMAL;      /* CI sent; RUN completes async on CC */
        }
        return 0;

    case DNET_BROKER_OP_SEND:
        if (!dnet_link_is_up(&eng->link)) {
            rsp->status = SS$_DEVOFFLINE;  /* honest: no link to send on */
            return 0;
        }
        if (dnet_engine_link_send(eng, req->data, req->datalen,
                                  frame_out, framecap, &flen, now) != DNET_ENGINE_OK) {
            rsp->status = SS$_ABORT;
            return 0;
        }
        *framelen = flen; *has_frame = 1;
        rsp->status = SS$_NORMAL;
        return 0;

    case DNET_BROKER_OP_RECV:
        if (eng->rx_datalen == 0) {
            rsp->status = SS$_ENDOFFILE;   /* nothing pending -- caller waits, we don't */
            return 0;
        }
        {
            uint16_t n = eng->rx_datalen;
            if (n > DNET_NSP_MAX_DATA) n = DNET_NSP_MAX_DATA;   /* defensive clamp */
            memcpy(rsp->data, eng->rx_data, n);
            rsp->datalen = n;
            eng->rx_datalen = 0;           /* consumed */
            rsp->status = SS$_NORMAL;
        }
        return 0;

    case DNET_BROKER_OP_CLOSE:
        if (dnet_engine_link_close(eng, DNET_LINK_REASON_NORMAL,
                                   frame_out, framecap, &flen, now) != DNET_ENGINE_OK) {
            rsp->status = SS$_ABORT;
            return 0;
        }
        *framelen = flen; *has_frame = 1;
        rsp->status = SS$_NORMAL;
        return 0;

    default:
        rsp->status = SS$_ILLIOFUNC;       /* unknown op -- honest refusal, no frame */
        return 0;
    }
}

/*
 * run_net_service_selftest (rd vms-22c, a1-2 integration) -- the host floor of
 * the NETACP broker SERVICE DISPATCH: dnet_broker_serve driving each op against
 * the real NSP link engine over a socketpair, with NO mailbox and NO executive.
 * Proves the request -> engine-op mapping + the correlation-matched response +
 * the never-crash-a-peer refusals, before any mailbox/kernel wiring:
 *   OP_OPEN's CI drives a real bring-up (peer accepts, link reaches RUN);
 *   OP_SEND's data segment is received by the peer byte-exact;
 *   OP_RECV returns the buffered inbound message (and SS$_ENDOFFILE when empty);
 *   OP_CLOSE's DI closes the peer's link;
 *   an unknown op and a too-short OPEN are refused honestly (no frame, no
 *   over-read), correlation still echoed. The --copy-transport-selftest pattern.
 */
static int run_net_service_selftest(void)
{
    printf("DECNETD-I-NETSERVICE, NETACP broker service dispatch: OPEN/SEND/RECV/"
           "CLOSE against the NSP link engine, correlation-matched, never-crash"
           " refusals (no executive, rd vms-22c)\n");
    int pass = 0, fail = 0;
#define NS_CHECK(c, msg) do { if (c) { pass++; printf("  PASS: %s\n", msg); } \
    else { fail++; printf("  FAIL: %s\n", msg); } } while (0)

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        fprintf(stderr, "DECNETD-E-NETSERVICE, socketpair failed: %s\n", strerror(errno));
        return 1;
    }
    const uint8_t hwL[6] = { 0x02,0,0,0,0,0x0a };
    const uint8_t hwR[6] = { 0x02,0,0,0,0,0x0b };
    struct dnet_engine L, R;
    if (dnet_engine_init(&L, 1, 10, "OVMXL", "EWA0", NULL, hwL, 0, 0, 0) != 0 ||
        dnet_engine_init(&R, 1, 11, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0) != 0) {
        fprintf(stderr, "DECNETD-E-NETSERVICE, engine init failed\n");
        close(sv[0]); close(sv[1]); return 1;
    }

    struct dnet_broker_req req; struct dnet_broker_rsp rsp;
    uint8_t frame[DNET_FRAME_MAX], rxbuf[DNET_FRAME_MAX], reply[DNET_FRAME_MAX];
    size_t flen = 0, rxlen = 0, rlen = 0; int has_reply = 0, has = 0;
    enum dnet_link_event ev = DNET_LINK_EV_NONE;
    uint32_t corr = 0;
    dnet_tick_t t = 10;

    /* 1) OP_OPEN via serve on L -> CI -> R accepts -> CC -> L link RUN. */
    memset(&req, 0, sizeof req);
    req.corr_id = dnet_broker_corr_next(&corr);
    req.op = DNET_BROKER_OP_OPEN;
    /* OP_OPEN payload: the NCB text. NETACP resolves the node; "1.11" is the
     * area.node literal sethost_resolve_target accepts directly (no node-DB
     * entry needed for the host proof); it resolves to the peer engine R. */
    {
        const char *ncb = "1.11::\"TASK=SVCTEST\"";
        req.datalen = (uint16_t)strlen(ncb);
        memcpy(req.data, ncb, req.datalen);
    }
    int sr = dnet_broker_serve(&L, &req, &rsp, frame, sizeof frame, &flen, &has, t++);
    NS_CHECK(sr == 0 && rsp.status == SS$_NORMAL && rsp.corr_id == req.corr_id && has == 1,
             "OP_OPEN: serve builds a CI frame, status NORMAL, correlation echoed");

    int up = has &&
        move_frame(sv[0], sv[1], frame, flen, rxbuf, sizeof rxbuf, &rxlen) == 0 &&
        dnet_engine_link_rx(&R, t++, rxbuf, rxlen, reply, sizeof reply, &rlen, &has_reply, &ev) == 0 &&
        ev == DNET_LINK_EV_CONNECT_IND &&
        dnet_engine_link_accept(&R, 0x2002, reply, sizeof reply, &rlen, t++) == 0 &&
        move_frame(sv[1], sv[0], reply, rlen, rxbuf, sizeof rxbuf, &rxlen) == 0 &&
        dnet_engine_link_rx(&L, t++, rxbuf, rxlen, frame, sizeof frame, &flen, &has_reply, &ev) == 0 &&
        ev == DNET_LINK_EV_CONNECT_CONF && dnet_link_is_up(&L.link) && dnet_link_is_up(&R.link);
    NS_CHECK(up, "OP_OPEN's CI drives the bring-up: peer accepts, the link reaches RUN both ends");

    /* 2) OP_SEND via serve on L -> data -> R receives it byte-exact. */
    req.corr_id = dnet_broker_corr_next(&corr);
    req.op = DNET_BROKER_OP_SEND;
    const char *smsg = "SERVICE-DISPATCH task payload 0123456789";
    req.datalen = (uint16_t)strlen(smsg);
    memcpy(req.data, smsg, req.datalen);
    has = 0;
    sr = dnet_broker_serve(&L, &req, &rsp, frame, sizeof frame, &flen, &has, t++);
    int sent = up && sr == 0 && rsp.status == SS$_NORMAL && rsp.corr_id == req.corr_id && has == 1 &&
        move_frame(sv[0], sv[1], frame, flen, rxbuf, sizeof rxbuf, &rxlen) == 0 &&
        dnet_engine_link_rx(&R, t++, rxbuf, rxlen, reply, sizeof reply, &rlen, &has_reply, &ev) == 0 &&
        ev == DNET_LINK_EV_DATA && R.rx_datalen == strlen(smsg) &&
        memcmp(R.rx_data, smsg, R.rx_datalen) == 0;
    if (sent && has_reply) {   /* absorb R's data-ack back on L */
        move_frame(sv[1], sv[0], reply, rlen, rxbuf, sizeof rxbuf, &rxlen);
        dnet_engine_link_rx(&L, t++, rxbuf, rxlen, frame, sizeof frame, &flen, &has_reply, &ev);
    }
    NS_CHECK(sent, "OP_SEND: serve builds a data frame the peer receives byte-exact (correlation echoed)");

    /* 3) OP_RECV via serve on R -> returns the buffered task message. */
    req.corr_id = dnet_broker_corr_next(&corr);
    req.op = DNET_BROKER_OP_RECV; req.datalen = 0;
    sr = dnet_broker_serve(&R, &req, &rsp, frame, sizeof frame, &flen, &has, t++);
    NS_CHECK(sr == 0 && rsp.status == SS$_NORMAL && rsp.corr_id == req.corr_id && has == 0 &&
             rsp.datalen == strlen(smsg) && memcmp(rsp.data, smsg, rsp.datalen) == 0,
             "OP_RECV: serve returns the buffered inbound task message to the reader (byte-exact)");
    req.corr_id = dnet_broker_corr_next(&corr);
    sr = dnet_broker_serve(&R, &req, &rsp, frame, sizeof frame, &flen, &has, t++);
    NS_CHECK(sr == 0 && rsp.status == SS$_ENDOFFILE,
             "OP_RECV with nothing pending returns the honest SS$_ENDOFFILE (never blocks here)");

    /* 4) OP_CLOSE via serve on L -> DI -> R's link goes CLOSED. */
    req.corr_id = dnet_broker_corr_next(&corr);
    req.op = DNET_BROKER_OP_CLOSE; req.datalen = 0; has = 0;
    sr = dnet_broker_serve(&L, &req, &rsp, frame, sizeof frame, &flen, &has, t++);
    int closed = sr == 0 && rsp.status == SS$_NORMAL && rsp.corr_id == req.corr_id && has == 1 &&
        move_frame(sv[0], sv[1], frame, flen, rxbuf, sizeof rxbuf, &rxlen) == 0 &&
        dnet_engine_link_rx(&R, t++, rxbuf, rxlen, reply, sizeof reply, &rlen, &has_reply, &ev) == 0 &&
        ev == DNET_LINK_EV_DISCONNECT && dnet_link_state_of(&R.link) == DNET_LINK_CLOSED;
    NS_CHECK(closed, "OP_CLOSE: serve builds a DI frame; the peer's link goes CLOSED");

    /* 5) NEVER-CRASH: an unknown op and a too-short OPEN are refused honestly. */
    req.corr_id = dnet_broker_corr_next(&corr);
    req.op = 0x7fffu; req.datalen = 0; has = 1;
    sr = dnet_broker_serve(&L, &req, &rsp, frame, sizeof frame, &flen, &has, t++);
    NS_CHECK(sr == 0 && rsp.status == SS$_ILLIOFUNC && has == 0 && rsp.corr_id == req.corr_id,
             "an unknown op is refused (SS$_ILLIOFUNC), no frame, correlation still echoed");
    req.corr_id = dnet_broker_corr_next(&corr);
    req.op = DNET_BROKER_OP_OPEN;
    req.data[0] = 200; req.data[1] = ':'; req.datalen = 2;   /* not an NCB: no node, no "::" */
    has = 1;
    sr = dnet_broker_serve(&L, &req, &rsp, frame, sizeof frame, &flen, &has, t++);
    NS_CHECK(sr == 0 && rsp.status == SS$_BADPARAM && has == 0,
             "OP_OPEN whose NCB is malformed is refused (BADPARAM, no over-read)");

    close(sv[0]); close(sv[1]);
    printf("DECNETD-I-NETSERVICE, %d passed, %d failed\n", pass, fail);
    if (fail == 0 && pass > 0) { printf("DECNETD-NET-SERVICE-SELFTEST: PASS\n"); return 0; }
    printf("DECNETD-NET-SERVICE-SELFTEST: FAIL\n");
    return 1;
#undef NS_CHECK
}

/*
 * run_net_mbx_selftest (rd vms-22c, a1-2 slice 2b) -- the T1 mailbox transport
 * round-trip on a REAL /dev/vms: the exec<->NETACP hop the record codec + service
 * dispatch ride, proven end to end through the executive mailbox (vms_mbx). A
 * single process plays both sides -- a client and NETACP -- so it needs no peer
 * node or datalink; it exercises exactly the mailbox plumbing:
 *   client  $CREMBXs its own reply mailbox, marshals a broker REQUEST carrying
 *           reply_unit, and writes it to NETACP's request mailbox;
 *   NETACP   reads the request (IO$M_NOW), BOUNDS-DECODES it (dnet_broker_req_
 *           decode), services it (dnet_broker_serve), ASSIGNS the client's reply
 *           mailbox BY UNIT (MBA<reply_unit>: -- the routing that delivers the
 *           response to the right waiter), and writes the RESPONSE;
 *   client  reads the response from ITS reply mailbox and CORRELATION-MATCHES it.
 * The op is OP_RECV on a fresh engine (rx buffer empty), so the honest
 * SS$_ENDOFFILE round-trips with no live link needed -- the point here is the
 * MAILBOX SEAM, not the link. FAIL-HONEST (Rule 9/INV-6, ONE RUNTIME): this test
 * does NOT probe for executive presence and does NOT skip. It always attempts the
 * real mailbox round-trip against /dev/vms; with no executive the $CREMBX calls
 * return SS$_NOSUCHDEV, the checks FAIL, and the test reports a TERMINAL honest
 * FAILURE (never a fake pass, never a silent userspace fallback). The decision of
 * WHERE to run it belongs to the harness, not this engine code: it is invoked
 * only by the booted acceptance battery (via run-on-rail's QEMU leg) where
 * /dev/vms is real -- it is NOT registered as a host ctest. The full task-to-task
 * e2e ($QIO _NET: -> qio_net_op -> NETACP -> a live link) is slice 2c.
 */
static int run_net_mbx_selftest(void)
{
    printf("DECNETD-I-NETMBX, T1 mailbox transport round-trip (client $CREMBX ->"
           " request -> NETACP read+serve -> reply-by-unit -> client correlation-"
           " match), against the booted executive /dev/vms (rd vms-22c)\n");

    int pass = 0, fail = 0;
/* A FAIL names the executive status that caused it (the VMS condition the
 * service returned), so a red on a rail is diagnosable from the log alone. */
#define MB_CHECK(c, msg) do { if (c) { pass++; printf("  PASS: %s\n", msg); } \
    else { fail++; printf("  FAIL: %s (last status %08X)\n", msg, (unsigned)st); } } while (0)

    const uint32_t MAXMSG = DNET_BROKER_REQ_MAX + 16;   /* > the 1024 default */
    uint32_t req_chan = 0, req_unit = 0;   char req_dev[64]   = {0};
    uint32_t rep_chan = 0, rep_unit = 0;   char rep_dev[64]   = {0};

    uint32_t st = vms_kif_mbx_create(0, MAXMSG, MAXMSG * 4,
                                     &req_chan, &req_unit, req_dev, sizeof req_dev);
    MB_CHECK(st & 1, "NETACP $CREMBX the request mailbox");
    st = vms_kif_mbx_create(0, MAXMSG, MAXMSG * 4,
                            &rep_chan, &rep_unit, rep_dev, sizeof rep_dev);
    MB_CHECK(st & 1, "client $CREMBX its own reply mailbox");

    if (pass >= 2) {
        /* client: marshal a request carrying reply_unit, write to NETACP's mbx. */
        struct dnet_broker_req req;
        memset(&req, 0, sizeof req);
        req.corr_id = 0xABCD1234u; req.owner_pid = 0x0000BEEFu;
        req.link_handle = 0x2001u; req.reply_unit = rep_unit;
        req.op = DNET_BROKER_OP_RECV; req.datalen = 0;
        uint8_t reqbuf[DNET_BROKER_REQ_MAX]; size_t reqlen = 0;
        dnet_broker_req_encode(&req, reqbuf, sizeof reqbuf, &reqlen);
        st = vms_kif_mbx_write(req_chan, reqbuf, (uint32_t)reqlen);
        MB_CHECK(st & 1, "client writes the request to NETACP's request mailbox");

        /* NETACP: read (IO$M_NOW) -> bounds-decode -> serve. */
        struct dnet_engine eng;
        const uint8_t hw[6] = { 0x02,0,0,0,0,0x0a };
        dnet_engine_init(&eng, 1, 10, "OVMXN", "EWA0", NULL, hw, 0, 0, 0);
        uint8_t rdbuf[DNET_BROKER_REQ_MAX]; uint32_t rdlen = 0;
        st = vms_kif_mbx_read(req_chan, rdbuf, sizeof rdbuf, &rdlen, 1 /*nowait*/);
        struct dnet_broker_req sreq;
        int dr = dnet_broker_req_decode(rdbuf, rdlen, &sreq);
        MB_CHECK((st & 1) && dr == DNET_BROKER_OK && sreq.corr_id == req.corr_id &&
                 sreq.reply_unit == rep_unit && sreq.op == DNET_BROKER_OP_RECV,
                 "NETACP reads + bounds-decodes the request (correlation + reply_unit intact)");

        struct dnet_broker_rsp srsp;
        uint8_t frame[DNET_FRAME_MAX]; size_t flen = 0; int has = 0;
        dnet_broker_serve(&eng, &sreq, &srsp, frame, sizeof frame, &flen, &has, 10);
        MB_CHECK(srsp.status == SS$_ENDOFFILE && srsp.corr_id == req.corr_id && has == 0,
                 "NETACP services it (RECV-empty -> honest ENDOFFILE, correlation echoed, no frame)");

        /* NETACP: assign the client's reply mailbox BY UNIT and write the response. */
        char rep_name[32];
        snprintf(rep_name, sizeof rep_name, "MBA%u:", (unsigned)sreq.reply_unit);
        uint32_t nrep_chan = 0;
        st = vms_kif_mbx_assign(rep_name, &nrep_chan);
        MB_CHECK(st & 1, "NETACP assigns the client's reply mailbox by unit (MBA<reply_unit>:)");
        uint8_t rspbuf[DNET_BROKER_RSP_MAX]; size_t rsplen = 0;
        dnet_broker_rsp_encode(&srsp, rspbuf, sizeof rspbuf, &rsplen);
        st = vms_kif_mbx_write(nrep_chan, rspbuf, (uint32_t)rsplen);
        MB_CHECK(st & 1, "NETACP writes the response to the reply mailbox");

        /* client: read the response from ITS reply mailbox, correlation-match. */
        uint8_t crbuf[DNET_BROKER_RSP_MAX]; uint32_t crlen = 0;
        st = vms_kif_mbx_read(rep_chan, crbuf, sizeof crbuf, &crlen, 0 /*wait*/);
        struct dnet_broker_rsp crsp;
        int cd = dnet_broker_rsp_decode(crbuf, crlen, &crsp);
        MB_CHECK((st & 1) && cd == DNET_BROKER_OK &&
                 dnet_broker_corr_match(req.corr_id, crsp.corr_id) &&
                 crsp.status == SS$_ENDOFFILE,
                 "client reads the response from ITS reply mailbox; correlation matches; status round-trips");

        if (nrep_chan) vms_kif_mbx_delmbx(nrep_chan);
    }

    if (req_chan) vms_kif_mbx_delmbx(req_chan);
    if (rep_chan) vms_kif_mbx_delmbx(rep_chan);

    printf("DECNETD-I-NETMBX, %d passed, %d failed\n", pass, fail);
    if (fail == 0 && pass > 0) { printf("DECNETD-NET-MBX-SELFTEST: PASS\n"); return 0; }
    printf("DECNETD-NET-MBX-SELFTEST: FAIL\n");
    return 1;
#undef MB_CHECK
}

/*
 * run_task_selftest (rd vms-dda) -- the host floor of the DECnet TASK-TO-TASK
 * client seam (the a1 ladder rung 1). It proves the generic NAMED-object logical
 * link an application task uses -- $ASSIGN NODE::"TASK=name" + $QIO -- over the
 * proven NSP link engine, with NO executive and NO CAP_NET_RAW:
 *   - the active side opens a link by TASK NAME (format-1 NAMED descriptor, not a
 *     hard-coded object number like CTERM 42 / FAL 17);
 *   - the passive side DECODES that named descriptor byte-exact (the addressing a
 *     NETACP object dispatcher matches against the object registry);
 *   - a request and a reply move BOTH DIRECTIONS byte-identical (the
 *     byte-transparent read/write pump the _NET: $QIO IO$_READVBLK/WRITEVBLK
 *     broker will expose -- Option 1, NETACP-brokered).
 * The DECnet analogue of --nsp-selftest (one-way, NULL descriptor) and the
 * foundation the _NET: $QIO broker sits on. CLEAN-ROOM (Rule 8): format 1 is the
 * published DNA named-task form; the link engine is OVMX's own.
 */
static int run_task_selftest(void)
{
    printf("DECNETD-I-TASKSELF, task-to-task logical link by NAMED object (TASK=):"
           " connect -> passive decodes the name -> bidirectional byte-verified"
           " message -> disconnect (no executive, rd vms-dda)\n");
    int pass = 0, fail = 0;
#define TK_CHECK(c, msg) do { if (c) { pass++; printf("  PASS: %s\n", msg); } \
    else { fail++; printf("  FAIL: %s\n", msg); } } while (0)

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        fprintf(stderr, "DECNETD-E-TASKSELF, socketpair failed: %s\n", strerror(errno));
        return 1;
    }
    const uint8_t hwL[6] = { 0x02,0,0,0,0,0x0a };
    const uint8_t hwR[6] = { 0x02,0,0,0,0,0x0b };
    struct dnet_engine L, R;
    if (dnet_engine_init(&L, 1, 10, "OVMXL", "EWA0", NULL, hwL, 0, 0, 0) != 0 ||
        dnet_engine_init(&R, 1, 11, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0) != 0) {
        fprintf(stderr, "DECNETD-E-TASKSELF, engine init failed\n");
        close(sv[0]); close(sv[1]); return 1;
    }

    uint8_t conn[192], frame[DNET_FRAME_MAX], rxbuf[DNET_FRAME_MAX], reply[DNET_FRAME_MAX];
    size_t clen = 0, flen = 0, rlen = 0, rxlen = 0; int has_reply = 0;
    enum dnet_link_event ev = DNET_LINK_EV_NONE;
    dnet_tick_t t = 10;
    const char *TASK = "TESTECHO";

    /* 1) active opens a NAMED task link -> CI -> passive CONNECT_IND. */
    int opened =
        dnet_cterm_sc_connect_build_task(TASK, "OVMXL", 0x021a, 0x2020, "", "", "",
                                         conn, sizeof conn, &clen) == 0 &&
        dnet_engine_link_open(&L, 1, 11, 0x2001, conn, clen, 1459, 1, DNET_NSP_VER_41,
                              frame, sizeof frame, &flen, t++) == 0 &&
        move_frame(sv[0], sv[1], frame, flen, rxbuf, sizeof rxbuf, &rxlen) == 0 &&
        dnet_engine_link_rx(&R, t++, rxbuf, rxlen, reply, sizeof reply, &rlen,
                            &has_reply, &ev) == 0 && ev == DNET_LINK_EV_CONNECT_IND;
    TK_CHECK(opened, "active opens a task-to-task link by name; passive sees CONNECT_IND");

    /* 2) passive decodes the destination as the NAMED task, byte-exact. */
    struct dnet_cterm_sc_connect sc;
    int named_ok = opened &&
        dnet_cterm_sc_connect_parse(R.link.conn_data, R.link.conn_len, &sc) == DNET_CTERM_OK &&
        sc.dst_format == DNET_SC_FMT_NAMED && strcmp(sc.dst_task, TASK) == 0;
    TK_CHECK(named_ok, "passive decodes the destination as NAMED task \"TESTECHO\" (format 1)");

    /* 3) passive accepts -> CC -> active link RUN. */
    int up = named_ok &&
        dnet_engine_link_accept(&R, 0x2002, reply, sizeof reply, &rlen, t++) == 0 &&
        move_frame(sv[1], sv[0], reply, rlen, rxbuf, sizeof rxbuf, &rxlen) == 0 &&
        dnet_engine_link_rx(&L, t++, rxbuf, rxlen, frame, sizeof frame, &flen,
                            &has_reply, &ev) == 0 && ev == DNET_LINK_EV_CONNECT_CONF &&
        dnet_link_is_up(&L.link) && dnet_link_is_up(&R.link);
    TK_CHECK(up, "passive accepts; the task-to-task link is RUN both ends");

    /* 4) active -> passive request, byte-identical (+ absorb the NSP ack). */
    const char *req = "TASK-REQUEST: ping payload 0123456789";
    int fwd = up &&
        dnet_engine_link_send(&L, (const uint8_t *)req, strlen(req), frame, sizeof frame, &flen, t++) == 0 &&
        move_frame(sv[0], sv[1], frame, flen, rxbuf, sizeof rxbuf, &rxlen) == 0 &&
        dnet_engine_link_rx(&R, t++, rxbuf, rxlen, reply, sizeof reply, &rlen, &has_reply, &ev) == 0 &&
        ev == DNET_LINK_EV_DATA && R.rx_datalen == strlen(req) &&
        memcmp(R.rx_data, req, R.rx_datalen) == 0 && has_reply &&
        move_frame(sv[1], sv[0], reply, rlen, rxbuf, sizeof rxbuf, &rxlen) == 0 &&
        dnet_engine_link_rx(&L, t++, rxbuf, rxlen, frame, sizeof frame, &flen, &has_reply, &ev) == 0 &&
        ev == DNET_LINK_EV_ACK;
    TK_CHECK(fwd, "active->passive request byte-identical over the link (+ NSP ack)");

    /* 5) passive -> active reply, byte-identical (the OTHER direction). */
    const char *resp = "TASK-REPLY: pong payload 9876543210";
    int rev = fwd &&
        dnet_engine_link_send(&R, (const uint8_t *)resp, strlen(resp), reply, sizeof reply, &rlen, t++) == 0 &&
        move_frame(sv[1], sv[0], reply, rlen, rxbuf, sizeof rxbuf, &rxlen) == 0 &&
        dnet_engine_link_rx(&L, t++, rxbuf, rxlen, frame, sizeof frame, &flen, &has_reply, &ev) == 0 &&
        ev == DNET_LINK_EV_DATA && L.rx_datalen == strlen(resp) &&
        memcmp(L.rx_data, resp, L.rx_datalen) == 0 && has_reply &&
        move_frame(sv[0], sv[1], frame, flen, rxbuf, sizeof rxbuf, &rxlen) == 0 &&
        dnet_engine_link_rx(&R, t++, rxbuf, rxlen, reply, sizeof reply, &rlen, &has_reply, &ev) == 0 &&
        ev == DNET_LINK_EV_ACK;
    TK_CHECK(rev, "passive->active reply byte-identical (bidirectional task data)");

    /* 6) active disconnects -> both CLOSED. */
    int closed = rev &&
        dnet_engine_link_close(&L, DNET_LINK_REASON_NORMAL, frame, sizeof frame, &flen, t++) == 0 &&
        move_frame(sv[0], sv[1], frame, flen, rxbuf, sizeof rxbuf, &rxlen) == 0 &&
        dnet_engine_link_rx(&R, t++, rxbuf, rxlen, reply, sizeof reply, &rlen, &has_reply, &ev) == 0 &&
        ev == DNET_LINK_EV_DISCONNECT && dnet_link_state_of(&R.link) == DNET_LINK_CLOSED &&
        move_frame(sv[1], sv[0], reply, rlen, rxbuf, sizeof rxbuf, &rxlen) == 0 &&
        dnet_engine_link_rx(&L, t++, rxbuf, rxlen, frame, sizeof frame, &flen, &has_reply, &ev) == 0 &&
        dnet_link_state_of(&L.link) == DNET_LINK_CLOSED;
    TK_CHECK(closed, "active disconnects; both ends CLOSED");

    close(sv[0]); close(sv[1]);
    printf("DECNETD-I-TASKSELF, %d passed, %d failed\n", pass, fail);
    if (fail == 0 && pass == 6) { printf("DECNETD-TASK-SELFTEST: PASS\n"); return 0; }
    printf("DECNETD-TASK-SELFTEST: FAIL\n");
    return 1;
#undef TK_CHECK
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
/* DT$_LA36, per the V7.3 node's own DCDEF (docs/oracle/vax73-starlet-defs/
 * DCDEF.txt). */
#define CT_DT_LA36  32

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
    {
        /* rd vms-2166: the port is recorded ON THE DEVICE in the executive,
         * where SHOW TERMINAL / SHOW PROCESS / F$GETDVI TT_ACCPORNAM read it --
         * not just in this process's memory. */
        char rpi[64] = "";
        uint32_t rst = vms_kif_terminal_getrpi(c.hs.devnam, rpi, sizeof rpi);
        CT_CHECK((rst & 1) && strcmp(rpi, c.hs.remote_port_info) == 0,
                 "the RTAn:'s executive device row carries the same Remote Port"
                 " Info (DVI$_TT_ACCPORNAM, read back by device name)");
        rst = vms_kif_terminal_getrpi("OPA0:", rpi, sizeof rpi);
        CT_CHECK(!(rst & 1) || rpi[0] == '\0',
                 "NEGCTL: the local console OPA0: carries no remote port info");
    }

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

    /* ---- 3b. THE ORIGINATING TERMINAL (rd vms-14b) -------------------------
     * A real VAX SET HOST conveys its terminal in its CTERM Initiate. Replay
     * the REAL bytes VAX1 sent from its LA36-typed console (tests/lab/captures/
     * decnet-sethost-inbound-20261005/sethost-inbound.wire.txt, 1.1->1.44 seg 1
     * and seg 2) through the host FSM NETACP runs, record the decode on this
     * session's RTAn: with the call NETACP makes, and read the device row back
     * from the executive -- the oracle (vax-rta-show-terminal.txt) is
     * Device_Type LA36 (DT$_LA36 = 32), Width 132, Page 0. */
    {
        static const uint8_t vax_bind_accept[17] = {
            0x04, 0x02, 0x04, 0x00, 0x07, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        static const uint8_t vax_initiate_seg[57] = {
            0x09, 0x00, 0x35, 0x00, 0x01, 0x00, 0x01, 0x04, 0x00, 0x07, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0xf2, 0x03, 0x02, 0x02, 0xc0,
            0x03, 0x03, 0x04, 0xfe, 0xff, 0xef, 0x00, 0x04, 0x18, 0x42, 0x20, 0x84,
            0x00, 0xa0, 0x02, 0x02, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        static struct dnet_cth cth;
        struct vms_devinfo info, con0, con1;
        uint32_t rst, dst;

        dnet_cth_init(&cth, 60);   /* NETACP_CTERM_IDLE_MS; the idle time plays no part here */
        CT_CHECK(dnet_cth_open(&cth) == DNET_CTH_OK &&
                     dnet_cth_rx(&cth, vax_bind_accept, sizeof vax_bind_accept, 0) == DNET_CTH_OK &&
                     dnet_cth_rx(&cth, vax_initiate_seg, sizeof vax_initiate_seg, 0) == DNET_CTH_OK &&
                     cth.peer.term.valid && cth.peer.term.devtype == CT_DT_LA36,
                 "the host FSM decodes the real VAX Initiate's terminal: DC$_TERM,"
                 " DT$_LA36 (32), width 132, page 0");
        rst = dnet_cterm_host_record_origin(&c.hs, &cth.peer.term);
        memset(&info, 0, sizeof(info));
        dst = vms_kif_getdvi_devnam(c.hs.devnam, &info);
        printf("  INFO: %s after record: status %08X, devtype %u, width %u, page %u\n",
               c.hs.devnam, (unsigned)rst, (unsigned)info.devtype,
               (unsigned)info.width, (unsigned)info.page);
        CT_CHECK((rst & 1) && (dst & 1) && info.devtype == CT_DT_LA36 &&
                     info.width == 132 && info.page == 0,
                 "the RTAn:'s EXECUTIVE device row carries the originating terminal"
                 " ($GETDVI from this process: device type LA36, width 132, page 0)");
        /* The characteristic set the oracle RTAn: shows (vax-rta-show-
         * terminal.txt): the conveyed TT$/TT2$ words decide No Broadcast,
         * Hardcopy and Line Editing (among all the others they carry);
         * Interactive, Set_speed and VMS Style Input are the minted values. */
        CT_CHECK((dst & 1) && info.devchar == (VMS_TTC_INTERACTIVE | VMS_TTC_ECHO |
                     VMS_TTC_TYPEAHEAD | VMS_TTC_TTSYNC | VMS_TTC_LOWERCASE |
                     VMS_TTC_WRAP | VMS_TTC_HARDCOPY | VMS_TTC_FULLDUP |
                     VMS_TTC_SET_SPEED | VMS_TTC_LINE_EDITING | VMS_TTC_INSERT_EDITING |
                     VMS_TTC_NUMERIC_KEYPAD | VMS_TTC_VMS_STYLE_INPUT),
                 "the RTAn:'s EXECUTIVE characteristics are the oracle RTAn:'s set"
                 " (No Broadcast, Hardcopy, Line Editing from the VAX's TT$/TT2$)");

        /* NEGCTL: a local terminal is never redefined through this door, and
         * stays exactly as it was. */
        memset(&con0, 0, sizeof(con0));
        memset(&con1, 0, sizeof(con1));
        (void)vms_kif_getdvi_devnam("OPA0:", &con0);
        rst = vms_kif_terminal_setchar("OPA0:", VMS_TERMCHAR_M_TYPE | VMS_TERMCHAR_M_WIDTH |
                                       VMS_TERMCHAR_M_PAGE | VMS_TERMCHAR_M_CHAR,
                                       CT_DT_LA36, 80, 66, VMS_TTC_HARDCOPY,
                                       VMS_TTC_BROADCAST);
        dst = vms_kif_getdvi_devnam("OPA0:", &con1);
        CT_CHECK(rst == SS$_IVDEVNAM && (dst & 1) && con1.devtype == 0 &&
                     con1.devtype == con0.devtype && con1.width == con0.width &&
                     con1.page == con0.page && con1.devchar == con0.devchar,
                 "NEGCTL: OPA0: refuses the RTAn: setter (SS$_IVDEVNAM) and keeps"
                 " device type Unknown and its own width/page/characteristics");
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

    /* ---- 6. Tear down and prove the device row went with the session ------ *
     *
     * dnet_cterm_host_close() is the link going down: it hangs up the PTY and
     * WITHDRAWS the RTAn: (VMS_IOCTL_TERM_DELETE). The executive deletes a
     * withdrawn unit when its LAST channel is released (rd vms-1875) -- and the
     * LOGINOUT session process bound to it (which has also used up its three
     * attempts) still holds that channel until it runs down. Deleting the row
     * any earlier is what freed it out from under that process's channel and
     * corrupted the kernel. So the property proven here is the faithful one:
     * the session process runs down, and the unit goes with it. Both are
     * asynchronous rundowns of ANOTHER process, so each is awaited against a
     * bound (the executive is asked, never assumed), then asserted. */
    {
        char devnam[DNET_CTERM_HOST_DEVNAM];
        struct vms_devinfo info;
        struct vms_procinfo pinfo;
        uint32_t spid = c.hs.session_pid;
        const struct timespec tick = { 0, 50 * 1000 * 1000 };   /* 50 ms */
        int ms, session_gone = 0, unit_gone = 0;

        snprintf(devnam, sizeof(devnam), "%s", c.hs.devnam);
        (void)dnet_cterm_host_close(&c.hs);

        for (ms = 0; ms < 30000 && !session_gone; ms += 50) {
            memset(&pinfo, 0, sizeof(pinfo));
            if ((vms_kif_getjpi_pid(spid, &pinfo) & 1) == 0)
                session_gone = 1;
            else
                nanosleep(&tick, NULL);
        }
        CT_CHECK(session_gone,
                 "the session process bound to the RTAn: runs down once its link"
                 " is torn down");

        for (ms = 0; ms < 5000 && !unit_gone; ms += 50) {
            memset(&info, 0, sizeof(info));
            if ((vms_kif_getdvi_devnam(devnam, &info) & 1) == 0)
                unit_gone = 1;
            else
                nanosleep(&tick, NULL);
        }
        CT_CHECK(unit_gone,
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

/* ---------------------------------------------------------------------------
 * The $ SET HOST client's link-independent half (rd vms-dda). The CTERM client
 * FSM, the local terminal channels and the prompt-gated input queue are the
 * same whichever way the logical link is reached: the standalone engine over
 * the datalink (a host/lab instrument), or -- the booted runtime -- a _NET:
 * channel whose link NETACP owns. Only sh_link_send differs.
 * ------------------------------------------------------------------------- */
struct netcli;
static uint32_t netcli_op(struct netcli *c, uint16_t op, const void *in, size_t inlen,
                          void *out, size_t outcap, size_t *xfer);

struct sh_ctx {
    struct dnet_cterm_session term;
    struct dnet_cterm_inq inq;
    int read_pending, passall_on, session_bound_ever, stdin_eof, rc, done;
    int skip_lf;                        /* CTERM Write "newline" flag state  */
    int quiet;                          /* a user's SET HOST: no DECNETD-I chatter */
    uint16_t ch_in, ch_out;
    /* the link: the standalone engine (eng/sock/ifindex) OR a _NET: channel */
    struct dnet_engine *eng;
    int sock;
    unsigned ifindex;
    struct netcli *nc;
    const char *where;                  /* circuit / "NETACP" for the log lines */
    uint8_t cpdu[DNET_CTERM_MAX_PDU];
};

static int sh_link_send(struct sh_ctx *x, const uint8_t *pdu, size_t n)
{
    if (x->nc) {
        size_t xf = 0;
        return (netcli_op(x->nc, DNET_BROKER_OP_SEND, pdu, n, NULL, 0, &xf) & 1) ? 0 : -1;
    }
    return cterm_link_send(x->eng, x->sock, x->ifindex, pdu, n, monotonic_sec());
}

static int sethost_send_queued_line(struct sh_ctx *x)
{
    uint8_t line[DNET_CTERM_MAX_DATA];
    size_t linelen = 0;
    if (!dnet_cterm_inq_dequeue(&x->inq, line, sizeof(line), &linelen))
        return 0;
    size_t clen = 0;
    if (dnet_cterm_found_read_data_build(line, linelen, 0x0d, x->cpdu, sizeof x->cpdu,
                                         &clen) != 0)
        return -1;
    if (sh_link_send(x, x->cpdu, clen) != 0)
        return -1;
    x->term.reads_sent++;
    return 1;
}

/* The link is RUN: arm the CTERM client FSM and wait for the host, which
 * speaks first (rd vms-6165). */
static void sethost_on_bound_msg(struct sh_ctx *x, const uint8_t *data, size_t len);

static void sethost_on_linkup(struct sh_ctx *x)
{
    if (dnet_cterm_client_open(&x->term) != 0) {
        fprintf(stderr, "DECNETD-E-CTERMOPEN, could not arm CTERM client foundation\n");
        x->rc = 1; x->done = 1;
    }
}

/* One CTERM segment from the host. */
static void sethost_on_data(struct sh_ctx *x, const uint8_t *data, size_t len)
{
    size_t clen = 0;
    if (dnet_cterm_state_of(&x->term) == DNET_CTERM_S_BINDING) {
        /* FOUNDATION PHASE (rd vms-6165): consume the host's foundation
         * message, then drain every client reply now due (client seg-1, then
         * the seg-2/3/4 burst). WIDTH/PAGE are the captured 132/24 so every
         * emitted byte is oracle-exact. */
        int prog = 0;
        if (dnet_cterm_client_found_rx(&x->term, data, len, &prog) != DNET_CTERM_OK)
            return;
        for (;;) {
            int frc = dnet_cterm_client_found_next(&x->term, 132, 24, x->cpdu,
                                                   sizeof x->cpdu, &clen);
            if (frc != DNET_CTERM_OK || clen == 0)
                break;
            if (sh_link_send(x, x->cpdu, clen) != 0) {
                fprintf(stderr, "DECNETD-E-FOUND, could not send a CTERM foundation reply\n");
                x->rc = 1; x->done = 1;
                break;
            }
        }
        if (!x->done && dnet_cterm_is_bound(&x->term) && !x->session_bound_ever) {
            x->session_bound_ever = 1;
            if (!x->quiet) {
                log_ts(stdout);
                printf(" DECNETD-I-BOUND, CTERM foundation negotiated -- terminal session"
                       " BOUND on %s\n", x->where);
                fflush(stdout);
            }
            /* Hand echo/editing to the REMOTE session: pass-all the LOCAL
             * terminal through the executive driver. */
            sethost_set_line(x->ch_in, 1);
            x->passall_on = 1;
        }
        return;
    }

    /* BOUND: real terminal I/O inside 09 Common Data messages. ONE segment
     * carries SEVERAL CTERM messages, each LENGTH-prefixed (AA-DY89A-TK); a
     * real VAX packs a whole screen of Writes into one. Each is handled on its
     * own -- reading the segment as one message put every following LENGTH
     * word on the screen as a stray character (seen live: OVMX SET HOST VAX1). */
    struct dnet_cth_cd_iter it;
    if (dnet_cth_cd_iter_init(&it, data, len) != DNET_CTH_OK)
        return;
    const uint8_t *sub;
    size_t sublen;
    while (!x->done && dnet_cth_cd_iter_next(&it, &sub, &sublen) == 1) {
        if (sublen == 0)
            continue;
        if (sub[0] == 0x07) {                     /* Write: render it properly */
            uint8_t shown[DNET_CTERM_MAX_DATA + 512];
            size_t sn = 0;
            x->term.writes_recv++;
            if (dnet_cterm_write_render(sub, sublen, &x->skip_lf, shown,
                                        sizeof shown, &sn) == 0 && sn) {
                struct _iosb iosb;
                (void)sys$qiow(0, x->ch_out, IO$_WRITEVBLK, &iosb, NULL, 0,
                               shown, (uint32_t)sn, 0, 0, 0, 0);
            }
            continue;
        }
        uint8_t one[4 + DNET_CTERM_MAX_DATA + 64];
        if (sublen + 4 > sizeof one)
            continue;
        one[0] = 0x09; one[1] = 0x00;
        one[2] = (uint8_t)(sublen & 0xff); one[3] = (uint8_t)(sublen >> 8);
        memcpy(one + 4, sub, sublen);
        sethost_on_bound_msg(x, one, sublen + 4);
    }
}

/* One BOUND-phase CTERM message (re-wrapped in its own 09 envelope). */
static void sethost_on_bound_msg(struct sh_ctx *x, const uint8_t *data, size_t len)
{
    size_t clen = 0;
    enum dnet_cterm_found_term_kind tk = DNET_CTERM_TK_NONE;
    uint8_t txt[DNET_CTERM_MAX_DATA];
    size_t txtlen = 0;
    uint8_t rhandle[4] = { 0, 0, 0, 0 };
    if (dnet_cterm_found_terminal_rx(data, len, &tk, txt, sizeof(txt), &txtlen,
                                     rhandle) != DNET_CTERM_OK)
        return;
    if (tk == DNET_CTERM_TK_WRITE) {
        x->term.writes_recv++;
        if (txtlen) {
            struct _iosb iosb;
            (void)sys$qiow(0, x->ch_out, IO$_WRITEVBLK, &iosb, NULL, 0,
                           txt, (uint32_t)txtlen, 0, 0, 0, 0);
        }
    } else if (tk == DNET_CTERM_TK_START_READ) {
        /* rd vms-6165: a 02-08 is PROMPT-AND-READ -- display the prompt text,
         * THEN answer with exactly one queued input line. One solicit -> one
         * line, never more, never before this arrives. If the queue has no
         * complete line yet, remember the solicit and satisfy it the moment one
         * becomes available. */
        x->term.writes_recv++;
        if (txtlen) {
            struct _iosb iosb;
            (void)sys$qiow(0, x->ch_out, IO$_WRITEVBLK, &iosb, NULL, 0,
                           txt, (uint32_t)txtlen, 0, 0, 0, 0);
        }
        int srr = sethost_send_queued_line(x);
        if (srr < 0) {
            fprintf(stderr, "DECNETD-E-READDATA, could not send CTERM Read Data\n");
            x->rc = 1; x->done = 1;
        } else {
            x->read_pending = (srr == 0);
        }
    } else if (tk == DNET_CTERM_TK_READ_ATTR) {
        /* Host solicited terminal characteristics: answer with the oracle
         * read-characteristics reply, echoing its handle. */
        if (dnet_cterm_found_client_readchar_build(rhandle, x->cpdu, sizeof x->cpdu,
                                                   &clen) == 0)
            (void)sh_link_send(x, x->cpdu, clen);
    }
    /* TK_OTHER / TK_NONE: NSP-ack only, nothing to display or answer. */
}

/* The local terminal is readable: read it through its VMS channel. */
static void sethost_on_term_input(struct sh_ctx *x)
{
    uint8_t inbuf[DNET_CTERM_MAX_DATA];
    struct _iosb iosb;
    /* Read the keystrokes through the VMS terminal channel ($QIO), never a raw
     * read on fd 0. poll() only told us bytes are ready. */
    uint32_t rst = sys$qiow(0, x->ch_in, IO$_READVBLK, &iosb, NULL, 0,
                            inbuf, (uint32_t)sizeof(inbuf), 0, 0, 0, 0);
    uint32_t rn = (rst & 1) ? iosb.iosb$l_dev_depend : 0;
    if ((rst & 1) && rn > 0) {
        /* rd vms-6165: local keystrokes are BUFFERED, never sent immediately --
         * CTERM input is prompt-gated. */
        (void)dnet_cterm_inq_feed(&x->inq, inbuf, (size_t)rn);
    } else {
        /* Local EOF or a channel error: stop polling for more input but KEEP
         * THE LINK UP -- whatever is already queued is still dequeued one line
         * per solicit, and the host's remaining output still drains. The
         * session ends on the host's Unbind, a link drop, or --duration --
         * NEVER on stdin EOF by itself (rd vms-6165 lab iter 2). */
        x->stdin_eof = 1;
        dnet_cterm_inq_eof(&x->inq);
        log_ts(stdout);
        printf(" DECNETD-I-EOF, local input closed -- %zu byte(s) still queued,"
               " draining remote output on %s\n", x->inq.len, x->where);
        fflush(stdout);
    }
    /* A host solicit may already be waiting on a line that was not available
     * yet -- satisfy it now if the queue (or EOF) supplied one. */
    if (x->read_pending) {
        int srr = sethost_send_queued_line(x);
        if (srr < 0) {
            fprintf(stderr, "DECNETD-E-READDATA, could not send CTERM Read Data\n");
            x->rc = 1; x->done = 1;
        } else if (srr == 1) {
            x->read_pending = 0;
        }
    }
}

/* Assign the LOCAL VMS terminal channels (the anti-LARP core): SYS$INPUT for
 * keystrokes, SYS$OUTPUT for screen writes. All terminal I/O goes through these
 * channels via $QIO -- never raw termios on fd 0/1. */
static int sethost_open_terminal(struct sh_ctx *x)
{
    if (!vms_pcb_get())
        vms_pcb_init(0);
    struct dsc$descriptor_s din, dout;
    sethost_mkdesc(&din, "SYS$INPUT:");
    sethost_mkdesc(&dout, "SYS$OUTPUT:");
    if (!(sys$assign(&din, &x->ch_in, 0, NULL) & 1) ||
        !(sys$assign(&dout, &x->ch_out, 0, NULL) & 1)) {
        fprintf(stderr, "DECNETD-E-NOTERMCHAN, could not $ASSIGN the local"
                        " terminal (SYS$INPUT/SYS$OUTPUT)\n");
        if (x->ch_in) sys$dassgn(x->ch_in);
        x->ch_in = 0;
        return -1;
    }
    return 0;
}

/* Restore the terminal, unbind a bound session, say %REM-S-END. The caller
 * then releases the link. */
static void sethost_finish(struct sh_ctx *x, const char *local_node)
{
    if (x->passall_on)
        sethost_set_line(x->ch_in, 0);
    if (dnet_cterm_is_bound(&x->term)) {
        size_t clen = 0;
        if (dnet_cterm_unbind(&x->term, DNET_CTERM_UNBIND_NORMAL, x->cpdu,
                              sizeof x->cpdu, &clen) == 0)
            (void)sh_link_send(x, x->cpdu, clen);
    }
    /* CONTROL RETURNS with the canonical VMS message (oracle docs/oracle/
     * vax-sethost-cterm.console.txt): the LOCAL node is the node control
     * returns to -- only once a session was actually established. */
    if (x->session_bound_ever) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "%%REM-S-END, control returned to node %s::\n", local_node);
        sethost_term_write(x->ch_out, msg);
    }
    if (x->quiet)
        return;
    log_ts(stdout);
    printf(" DECNETD-I-SETHOSTEND, SET HOST session ended: cterm writes_recv=%lu"
           " reads_sent=%lu on %s\n", x->term.writes_recv, x->term.reads_sent, x->where);
    fflush(stdout);
}

static int sethost_ctx_init(struct sh_ctx *x)
{
    memset(x, 0, sizeof *x);
    if (dnet_cterm_session_init(&x->term, DNET_CTERM_ROLE_TERMINAL) != 0) {
        fprintf(stderr, "DECNETD-E-CTERMINIT, terminal session init failed\n");
        return -1;
    }
    /* rd vms-6165: CTERM input is PROMPT-DRIVEN -- buffered here, released ONE
     * LINE PER HOST SOLICIT. */
    dnet_cterm_inq_init(&x->inq);
    return 0;
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
    static struct sh_ctx x;
    if (sethost_ctx_init(&x) != 0)
        return 1;
    x.eng = eng; x.sock = sock; x.ifindex = ifindex;
    static char where[DNET_DEVNAME_MAX + 16];
    snprintf(where, sizeof where, "circuit %s", eng->circuit);
    x.where = where;
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

    if (sethost_open_terminal(&x) != 0)
        return 1;
    int term_fd = vms$$chan_to_fd(x.ch_in);   /* poll() readiness only */

    uint8_t frame[DNET_FRAME_MAX], rxbuf[DNET_FRAME_MAX];
    size_t flen = 0;
    dnet_tick_t now = monotonic_sec();
    if (dnet_engine_link_open(eng, parea, pnode, 0x2001, sc, sclen, 1459, 1,
                              DNET_NSP_VER_41, frame, sizeof(frame), &flen, now)
            != DNET_ENGINE_OK) {
        fprintf(stderr, "DECNETD-E-NOCONNECT, could not open a logical link"
                        " to %u.%u\n", parea, pnode);
        sys$dassgn(x.ch_in); sys$dassgn(x.ch_out);
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

    while (!g_stop && !x.done) {
        now = monotonic_sec();
        dnet_periodic(eng, sock, ifindex, now);

        /* Connect-Initiate give-up: the FSM closed the link before we ever
         * bound -- the peer never answered. Report honestly and stop. */
        if (eng->link_active &&
            dnet_link_state_of(&eng->link) == DNET_LINK_CLOSED &&
            dnet_cterm_state_of(&x.term) == DNET_CTERM_S_CLOSED) {
            log_ts(stdout);
            printf(" DECNETD-W-UNREACH, peer %u.%u did not answer -- SET HOST"
                   " abandoned\n", parea, pnode);
            fflush(stdout);
            eng->link_active = 0;
            x.rc = 1;
            break;
        }

        struct pollfd pfd[2];
        int nfd = 0;
        pfd[nfd].fd = sock;          pfd[nfd].events = POLLIN; pfd[nfd].revents = 0; nfd++;
        if (dnet_cterm_is_bound(&x.term) && !x.stdin_eof && term_fd >= 0) {
            pfd[nfd].fd = term_fd;   pfd[nfd].events = POLLIN; pfd[nfd].revents = 0; nfd++;
        }
        int pr = poll(pfd, (nfds_t)nfd, 1000);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "DECNETD-E-POLL, poll failed: %s\n", strerror(errno));
            x.rc = 1;
            break;
        }
        if (pr == 0)
            continue;

        if (pfd[0].revents & POLLIN) {
            int ev = dnet_recv_route(eng, sock, ifindex, now, rxbuf, sizeof(rxbuf));
            if (ev < 0) {
                fprintf(stderr, "DECNETD-E-RECVFAIL, recv failed: %s\n",
                        strerror(errno));
                x.rc = 1;
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
                 * data. This is the NSP layer; foundation CONTENT stays
                 * host-first. */
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
                        x.rc = 1; x.done = 1;
                        break;
                    }
                }
                sethost_on_linkup(&x);
                break;
            case DNET_LINK_EV_DATA:
                sethost_on_data(&x, eng->rx_data, eng->rx_datalen);
                break;
            case DNET_LINK_EV_DISCONNECT:
            case DNET_LINK_EV_DISCONNECT_CONF:
                log_ts(stdout);
                printf(" DECNETD-I-LINKDOWN, logical link closed on circuit %s\n",
                       eng->circuit);
                fflush(stdout);
                eng->link_active = 0;
                x.done = 1;
                break;
            default:
                break;
            }
        }

        if (nfd > 1 && (pfd[1].revents & (POLLIN | POLLHUP)))
            sethost_on_term_input(&x);
    }

    sethost_finish(&x, eng->node_name);
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
    sys$dassgn(x.ch_in);
    sys$dassgn(x.ch_out);
    return x.rc;
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

/* Ship one DAP segment (one NSP data segment) on the link. */
static int fal_xport_send(void *ctx, const uint8_t *seg, size_t seglen)
{
    struct fal_xport *x = ctx;
    uint8_t frame[DNET_FRAME_MAX];
    size_t  flen = 0;
    if (dnet_engine_link_send(x->eng, seg, seglen, frame, sizeof frame, &flen,
                              (*x->tick)++) != 0)
        return -1;
    if (write(x->wfd, frame, flen) != (ssize_t)flen) return -1;
    return 0;
}

/* Receive the next NSP data segment's payload (it may carry several blocked
 * DAP messages; dnet_fal splits them). Absorbs NSP acks and ships the ack owed
 * for a received data segment (real NSP flow). Returns 0 with the payload, or
 * -1 on a closed link. */
static int fal_xport_recv(void *ctx, uint8_t *buf, size_t cap, size_t *outlen)
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
            if (x->eng->rx_datalen > cap) return -1;
            memcpy(buf, x->eng->rx_data, x->eng->rx_datalen);
            *outlen = x->eng->rx_datalen;
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

/* The client is done: disconnect the link (NSP Disconnect Initiate) so a FAL
 * server waiting for its next access sees the link close and returns -- the
 * way a real accessor ends a multi-access FAL session (spec 5.1). */
static void fal_hangup(struct fal_xport *cxp)
{
    uint8_t frame[DNET_FRAME_MAX]; size_t flen = 0;
    if (dnet_engine_link_close(cxp->eng, DNET_LINK_REASON_NORMAL, frame, sizeof frame,
                               &flen, (*cxp->tick)++) == 0)
        (void)write(cxp->wfd, frame, flen);
}

/* Thread body: the FAL server side of one accepted session. */
struct fal_server_arg { struct fal_xport xp; uint32_t status; };
static void *fal_server_thread(void *v)
{
    struct fal_server_arg *a = v;
    struct dnet_dap_transport t = { .send = fal_xport_send, .recv = fal_xport_recv, .ctx = &a->xp };
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
            struct dnet_dap_transport ct = { .send = fal_xport_send, .recv = fal_xport_recv, .ctx = &cxp };
            uint32_t cst = dnet_fal_client_get("OVMXR::DKA0:[X]NOPE.TXT",
                                               "DKA0:[X]LOCAL.TXT", &ct);
            fal_hangup(&cxp);
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
/* Byte-verify a file record for record through RMS $OPEN/$GET -- the same
 * reader the FAL server uses, which frames by the file's own record format (a
 * GET stores VAR records; rms_textfile_* reads only stream text). */
static int fal_file_matches(const char *spec, const char *const *lines, int nlines)
{
    void *rf = NULL;
    if (dnet_fal_ropen(spec, &rf, NULL, NULL) != 0) return 0;
    uint8_t buf[DNET_DAP_MAX_REC]; size_t n = 0; int i = 0, ok = 1, g;
    while ((g = dnet_fal_rget(rf, buf, sizeof buf, &n)) == 1) {
        if (i >= nlines || n != strlen(lines[i]) || memcmp(buf, lines[i], n) != 0) { ok = 0; break; }
        i++;
    }
    (void)dnet_fal_rclose(rf);
    return ok && g == 0 && i == nlines;
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
            struct dnet_dap_transport ct = { .send = fal_xport_send, .recv = fal_xport_recv, .ctx = &cxp };
            uint32_t cst = dnet_fal_client_put(SRC, DEST, &ct);
            fal_hangup(&cxp);
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
            struct dnet_dap_transport ct = { .send = fal_xport_send, .recv = fal_xport_recv, .ctx = &cxp };
            uint32_t cst = dnet_fal_client_get(DEST, BACK, &ct);
            fal_hangup(&cxp);
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

/*
 * ============== --fal-proc-accept-test (rd vms-d85, R4 G3) ==================
 * The inbound FAL access runs in a NETWORK SERVER PROCESS with the
 * AUTHENTICATED user's identity, never NETACP's (dnet_fal_proc.h). Booted
 * battery only (needs /dev/vms + the mounted SYSUAF + SYS$SYSTEM:FAL.EXE); it
 * FAILS honestly anywhere else (Rule 9, INV-6). For each case: an object-17
 * link over a socketpair, connect-time auth (dnet_fal_connect_auth_id), then
 * NETACP's real path -- dnet_fal_proc_start $CREPRCs FAL.EXE with the user's
 * UIC + default privileges and this test pumps NSP segments <-> its mailboxes
 * exactly as the live NETACP will. The differential is the proof: the SAME
 * file through the SAME path is served to SYSTEM and REFUSED to GUEST, and the
 * server process's executive row carries the user's UIC.
 */
struct falp_pump {
    struct dnet_engine *R; int fd; dnet_tick_t *tick;
    struct dnet_fal_proc *fp;
    volatile int stop;
    uint32_t exit_status; int got_exit;
    int server_lost;          /* the server died/hung without EXIT */
};

static void *falp_pump_thread(void *v)
{
    struct falp_pump *pp = v;
    uint8_t rxbuf[DNET_FRAME_MAX], reply[DNET_FRAME_MAX], seg[DNET_FAL_SEG_MAX];
    int idle_after_exit = 0;
    unsigned iter = 0;
    while (!pp->stop) {
        /* Never hang the caller: a server process that died (or never got
         * its image running) without reporting EXIT ends the session -- the
         * shutdown unblocks the client's read with EOF -- and so does a hard
         * deadline (~60 s of 10 ms polls). */
        if ((++iter % 50) == 0 && !pp->got_exit &&
            (!dnet_fal_proc_alive(pp->fp) || iter > 6000)) {
            pp->server_lost = 1;
            shutdown(pp->fd, SHUT_RDWR);
            break;
        }
        struct pollfd pfd = { pp->fd, POLLIN, 0 };
        if (poll(&pfd, 1, 10) > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(pp->fd, rxbuf, sizeof rxbuf);
            if (n <= 0) break;
            size_t rlen = 0; int has = 0; enum dnet_link_event ev = DNET_LINK_EV_NONE;
            if (dnet_engine_link_rx(pp->R, (*pp->tick)++, rxbuf, (size_t)n, reply,
                                    sizeof reply, &rlen, &has, &ev) == 0) {
                if (has) (void)write(pp->fd, reply, rlen);
                if (ev == DNET_LINK_EV_DATA)
                    (void)dnet_fal_proc_put(pp->fp, pp->R->rx_data, pp->R->rx_datalen);
                if (ev == DNET_LINK_EV_DISCONNECT) break;
            }
        }
        size_t slen = 0; uint32_t xst = 0;
        int r = dnet_fal_proc_poll(pp->fp, seg, sizeof seg, &slen, &xst);
        if (r == 1) {
            uint8_t frame[DNET_FRAME_MAX]; size_t flen = 0;
            if (dnet_engine_link_send(pp->R, seg, slen, frame, sizeof frame, &flen,
                                      (*pp->tick)++) == 0)
                (void)write(pp->fd, frame, flen);
        } else if (r == 2) {
            pp->exit_status = xst; pp->got_exit = 1;
        } else if (r < 0) {
            break;
        }
        if (pp->got_exit && ++idle_after_exit > 50) break;   /* drained */
    }
    return NULL;
}

/* One FAL access through the network-server-process path. Returns the client
 * status; *srv_uic gets the server process's executive UIC (0 if none).
 * op: FALP_PUT / FALP_GET (remote, local), FALP_ERASE (remote), FALP_RENAME
 * (remote -> local as the NEW remote name); for ERASE / RENAME the remote's
 * refusal STATUS lands in *stscode / *stv (rd vms-277a). */
enum { FALP_PUT = 0, FALP_GET = 1, FALP_ERASE = 2, FALP_RENAME = 3, FALP_DIRLIST = 4 };
static char falp_names[512];      /* FALP_DIRLIST: the NAMEs received */
static uint32_t falp_session_op(const char *user, const char *pw, int op,
                                const char *remote, const char *local,
                                uint32_t *auth_out, uint32_t *srv_uic, uint32_t *srv_exit,
                                uint16_t *stscode, uint64_t *stv)
{
    const uint8_t hwL[6] = { 0x02,0,0,0,0,0x0a };
    const uint8_t hwR[6] = { 0x02,0,0,0,0,0x0b };
    int sv[2];
    *srv_uic = 0; *srv_exit = 0; *auth_out = 0;
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) return SS$_ABORT;
    struct dnet_engine L, R; dnet_tick_t tick = 500;
    dnet_engine_init(&L, 1, 10, "OVMXL", "EWA0", NULL, hwL, 0, 0, 0);
    dnet_engine_init(&R, 1, 11, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0);

    uint8_t conn[128], frame[DNET_FRAME_MAX], rxbuf[DNET_FRAME_MAX], reply[DNET_FRAME_MAX];
    size_t clen = 0, flen = 0, rxlen = 0, rlen = 0; int has = 0;
    enum dnet_link_event ev = DNET_LINK_EV_NONE;
    uint32_t cst = SS$_ABORT;
    if (dnet_cterm_sc_connect_build(DNET_OBJ_FAL, "OVMXL", 0x021a, 0x2020, user, pw, "",
                                    conn, sizeof conn, &clen) != 0 ||
        dnet_engine_link_open(&L, 1, 11, 0x2001, conn, clen, 1459, 1, DNET_NSP_VER_41,
                              frame, sizeof frame, &flen, tick++) != 0 ||
        move_frame(sv[0], sv[1], frame, flen, rxbuf, sizeof rxbuf, &rxlen) != 0 ||
        dnet_engine_link_rx(&R, tick++, rxbuf, rxlen, reply, sizeof reply, &rlen,
                            &has, &ev) != 0 || ev != DNET_LINK_EV_CONNECT_IND)
        goto out;

    struct dnet_fal_identity id;
    *auth_out = dnet_fal_connect_auth_id(R.link.conn_data, R.link.conn_len, &id);
    if (*auth_out != SS$_NORMAL) { cst = *auth_out; goto out; }

    struct dnet_fal_proc fp;
    uint32_t pst = dnet_fal_proc_start(&fp, id.uic, id.def_privs, id.username,
                                       id.default_dir);
    if (!(pst & 1)) {
        printf("  NOTE: FAL server process not created (status %08X at %s)\n", (unsigned)pst,
               fp.fail_stage ? fp.fail_stage : "?");
        cst = pst; goto out;
    }
    {
        struct vms_procinfo pi; memset(&pi, 0, sizeof pi);
        if (vms_kif_getjpi_pid(fp.pid, &pi) & 1) *srv_uic = pi.uic;
    }
    if (dnet_engine_link_accept(&R, 0x2002, reply, sizeof reply, &rlen, tick++) != 0 ||
        move_frame(sv[1], sv[0], reply, rlen, rxbuf, sizeof rxbuf, &rxlen) != 0 ||
        dnet_engine_link_rx(&L, tick++, rxbuf, rxlen, frame, sizeof frame, &flen,
                            &has, &ev) != 0 || ev != DNET_LINK_EV_CONNECT_CONF) {
        dnet_fal_proc_close(&fp); goto out;
    }

    struct falp_pump pp = { &R, sv[1], &tick, &fp, 0, 0, 0, 0 };
    pthread_t th;
    pthread_create(&th, NULL, falp_pump_thread, &pp);
    struct fal_xport cxp = { &L, sv[0], sv[0], &tick };
    struct dnet_dap_transport ct = { .send = fal_xport_send, .recv = fal_xport_recv, .ctx = &cxp };
    switch (op) {
    case FALP_GET:    cst = dnet_fal_client_get(remote, local, &ct); break;
    case FALP_PUT:    cst = dnet_fal_client_put(local, remote, &ct); break;
    case FALP_ERASE:  cst = dnet_fal_client_erase(remote, &ct, stscode, stv); break;
    case FALP_DIRLIST: cst = dnet_fal_client_dirlist(remote, &ct, falp_names, sizeof falp_names,
                                                      stscode, stv); break;
    default:          cst = dnet_fal_client_rename(remote, local, &ct, stscode, stv); break;
    }
    for (int i = 0; i < 300 && !pp.got_exit; i++) {          /* <= 3 s for EXIT */
        struct timespec ts = { 0, 10 * 1000 * 1000 }; nanosleep(&ts, NULL);
    }
    pp.stop = 1;
    pthread_join(th, NULL);
    *srv_exit = pp.got_exit ? pp.exit_status : 0;
    dnet_fal_proc_close(&fp);
out:
    close(sv[0]); close(sv[1]);
    return cst;
}

static uint32_t falp_session(const char *user, const char *pw, int is_get,
                             const char *remote, const char *local,
                             uint32_t *auth_out, uint32_t *srv_uic, uint32_t *srv_exit)
{
    return falp_session_op(user, pw, is_get ? FALP_GET : FALP_PUT, remote, local,
                           auth_out, srv_uic, srv_exit, NULL, NULL);
}

/* The persona fixture must be genuinely SYSTEM-only: protection
 * (S:RWED,O:RWED,G,W) -- ODS-2 fileprot 0xFF00, a set bit denies -- set by the
 * ACP at CREATE. A default-protected file is W:RE, and VMS lets GUEST read
 * that, so a GUEST refusal on it would be wrong, not a proof. */
static int falp_write_private(const char *spec, const char *line)
{
    uint32_t st = 0;
    rms_file_t *h = rms_open_named_handle_kind_prot(spec, 1, 1, ODS2_FK_DATA_STMLF,
                                                    0xFF00u, &st);
    if (!h) return -1;
    int rc = (rms_io_write_exact(h, line, strlen(line)) == 0 &&
              rms_io_write_exact(h, "\n", 1) == 0) ? 0 : -1;
    if (rc == 0) rms_io_fsync(h);
    rms_close_named_handle(h);
    return rc;
}

static int run_fal_proc_accept_test(void)
{
    printf("DECNETD-I-FALPROC, inbound FAL access runs in a FAL.EXE server process"
           " with the AUTHENTICATED user's UIC + privileges, never NETACP's"
           " (rd vms-d85, R4 G3)\n");
    if (!dnet_fal_proc_image_present()) {
        printf("DECNETD-I-FALPROC-NOIMAGE, SYS$SYSTEM:FAL.EXE is not on this system disk:"
               " no FAL server process can be created, so the persona proof cannot run here\n");
        printf("DECNETD-FAL-PROC-ACCEPT: NOIMAGE\n");
        return 1;
    }
    int pass = 0, fail = 0;
#define FP_CHECK(c, msg) do { if (c) { pass++; printf("  PASS: %s\n", msg); } \
    else { fail++; printf("  FAIL: %s\n", msg); } } while (0)

    const char *PRIV  = "SYS$SYSROOT:[SYSMGR]FALP_PRIV.TXT";
    const char *EVIL  = "SYS$SYSROOT:[SYSMGR]FALP_EVIL.TXT";
    const char *LOCAL = "SYS$SYSROOT:[SYSMGR]FALP_LOCAL.TXT";
    static const char *lines[] = { "FAL persona proof: a SYSTEM-owned record" };
    FP_CHECK(falp_write_private(PRIV, lines[0]) == 0,
             "a SYSTEM-only file (S:RWED,O:RWED,G,W) is laid down in SYS$SYSROOT:[SYSMGR] via RMS");

    uint32_t auth = 0, uic = 0, xst = 0, st;

    /* (1) SYSTEM reads it through the FAL server process: the path works and
     * the server runs as [1,4]. */
    st = falp_session("SYSTEM", "MANAGER", 1, PRIV, LOCAL, &auth, &uic, &xst);
    FP_CHECK(auth == SS$_NORMAL, "SYSTEM/MANAGER authenticates at connect");
    FP_CHECK(uic == ((1u << 16) | 4u),
             "the FAL server process for SYSTEM runs with UIC [1,4] (executive row)");
    FP_CHECK(st == SS$_NORMAL && fal_file_matches(LOCAL, lines, 1),
             "SYSTEM GETs the file through the FAL server process, records BYTE-MATCH");

    /* (2) GUEST through the SAME path: the server runs as GUEST and the
     * executive ACP refuses the file it cannot see. */
    st = falp_session("GUEST", "GUEST", 1, PRIV, LOCAL, &auth, &uic, &xst);
    FP_CHECK(auth == SS$_NORMAL, "GUEST/GUEST authenticates at connect");
    if (auth != SS$_NORMAL)       /* vms-330: say WHY (this proof fails intermittently in CI) */
        printf("  NOTE: GUEST connect auth status %08X, session status %08X\n",
               (unsigned)auth, (unsigned)st);
    FP_CHECK(uic == ((128u << 16) | 129u),
             "the FAL server process for GUEST runs with GUEST's UIC [128,129], NOT NETACP's");
    FP_CHECK(st != SS$_NORMAL,
             "GUEST's GET of a SYSTEM-only file is REFUSED -- the access is checked against the user, not the daemon");
    st = falp_session("GUEST", "GUEST", 1, "SYS$SYSTEM:SYSUAF.DAT", LOCAL, &auth, &uic, &xst);
    FP_CHECK(st != SS$_NORMAL, "GUEST cannot read SYS$SYSTEM:SYSUAF.DAT through FAL");

    /* (3) GUEST cannot write into a SYSTEM directory either. */
    static const char *evil[] = { "written by GUEST over FAL" };
    int wl = rms_textfile_write_line(LOCAL, evil[0]);
    if (wl != 0)
        printf("  NOTE: rms_textfile_write_line(%s) = %d, errno %d (%s)\n", LOCAL, wl, errno, strerror(errno));
    FP_CHECK(wl == 0, "a local source for the PUT exists");
    st = falp_session("GUEST", "GUEST", 0, EVIL, LOCAL, &auth, &uic, &xst);
    rms_textfile_t *chk = rms_textfile_open(EVIL);
    FP_CHECK(st != SS$_NORMAL && chk == NULL,
             "GUEST's PUT into SYS$SYSROOT:[SYSMGR] is REFUSED and no file is created");
    if (chk) rms_textfile_close(chk);

    /* (4) A bad password never creates a server process at all. */
    st = falp_session("GUEST", "WRONGPW", 1, PRIV, LOCAL, &auth, &uic, &xst);
    FP_CHECK(auth == SS$_INVLOGIN && uic == 0,
             "a bad password is refused at connect and NO FAL server process is created");

    /* (5) rd vms-277a: remote DELETE and RENAME through the SAME server
     * process. The refusals are the executive ACP's verdict on GUEST's
     * identity, and the STATUS the client receives must be the bytes a real
     * VMS V7.3 FAL sent for the same refusal
     * (tests/lab/captures/decnet-fal-verbs-20261008): DELETE = MAC 4 / MIC PRV
     * (0x4055) + STV 0x24, RENAME = MAC 4 / MIC RMV (0x405f) without STV. Then
     * SYSTEM, which owns the files, deletes and renames them, read back
     * through RMS. */
    {
        const char *DEL = "SYS$SYSROOT:[SYSMGR]FALP_DEL.TXT";
        const char *REN = "SYS$SYSROOT:[SYSMGR]FALP_REN.TXT";
        const char *RENAMED = "SYS$SYSROOT:[SYSMGR]FALP_RENAMED.TXT";
        const char *STOLEN = "SYS$SYSROOT:[SYSMGR]FALP_STOLEN.TXT";
        static const char *dl[] = { "FAL verbs: a SYSTEM-only file to delete" };
        static const char *rl[] = { "FAL verbs: a SYSTEM-only file to rename" };
        uint32_t s1 = 0, s2 = 0;
        (void)dnet_fal_erase(RENAMED, &s1, &s2);        /* a previous run's */
        FP_CHECK(falp_write_private(DEL, dl[0]) == 0 && falp_write_private(REN, rl[0]) == 0,
                 "SYSTEM-only files (S:RWED,O:RWED,G,W) to DELETE and RENAME are laid down via RMS");
        uint16_t sc = 0; uint64_t sv = 0;

        st = falp_session_op("GUEST", "GUEST", FALP_ERASE, DEL, NULL, &auth, &uic, &xst, &sc, &sv);
        printf("  NOTE: GUEST DELETE -> client %08X, STATUS %04X STV %llX\n",
               (unsigned)st, (unsigned)sc, (unsigned long long)sv);
        FP_CHECK(st != SS$_NORMAL && sc == 0x4055 && sv == 0x24,
                 "GUEST's remote DELETE of a SYSTEM-only file is REFUSED by the executive with the VAX FAL's STATUS 0x4055 (RMS-E-PRV) STV 0x24 (SS$_NOPRIV)");
        {
            uint32_t rs = 0; void *rh = NULL;
            int ro = dnet_fal_ropen_st(DEL, &rh, NULL, NULL, &rs);
            if (ro == 0) (void)dnet_fal_rclose(rh);
            printf("  NOTE: after GUEST DELETE, $OPEN %s -> %s (RMS %08X)\n", DEL,
                   ro == 0 ? "opens" : "refused", (unsigned)rs);
        }
        FP_CHECK(fal_file_matches(DEL, dl, 1), "the file GUEST tried to delete is still there, intact");

        st = falp_session_op("GUEST", "GUEST", FALP_RENAME, REN, STOLEN, &auth, &uic, &xst, &sc, &sv);
        printf("  NOTE: GUEST RENAME -> client %08X, STATUS %04X STV %llX\n",
               (unsigned)st, (unsigned)sc, (unsigned long long)sv);
        void *rf = NULL;
        int stolen = (dnet_fal_ropen(STOLEN, &rf, NULL, NULL) == 0);
        if (stolen) (void)dnet_fal_rclose(rf);
        FP_CHECK(st != SS$_NORMAL && sc == 0x405f && sv == 0,
                 "GUEST's remote RENAME of a SYSTEM-only file is REFUSED by the executive with the VAX FAL's STATUS 0x405f (RMS-F-RMV), no STV");
        FP_CHECK(!stolen && fal_file_matches(REN, rl, 1),
                 "the refused RENAME moved nothing: the old name still holds the file, the new name does not exist");

        {   /* diagnostic: the resultant the server will rename by */
            void *sx = NULL; char r[256] = "";
            if (dnet_fal_search_begin(REN, &sx) == 0) {
                if (dnet_fal_search_next(sx, r, sizeof r) != 0) r[0] = '\0';
                dnet_fal_search_end(sx);
            }
            printf("  NOTE: %s resolves to '%s'\n", REN, r);
        }
        st = falp_session_op("SYSTEM", "MANAGER", FALP_RENAME, REN, RENAMED, &auth, &uic, &xst, &sc, &sv);
        printf("  NOTE: SYSTEM RENAME -> client %08X, STATUS %04X STV %llX\n",
               (unsigned)st, (unsigned)sc, (unsigned long long)sv);
        uint32_t ors = 0;
        int old_gone = (dnet_fal_ropen_st(REN, &rf, NULL, NULL, &ors) != 0);
        if (!old_gone) (void)dnet_fal_rclose(rf);
        printf("  NOTE: after SYSTEM RENAME, $OPEN old -> RMS %08X; new name byte-match %d\n",
               (unsigned)ors, fal_file_matches(RENAMED, rl, 1));
        FP_CHECK(st == SS$_NORMAL && old_gone && fal_file_matches(RENAMED, rl, 1),
                 "SYSTEM renames its file through the FAL server process: RMS reads the records under the NEW name and the old name is gone");

        st = falp_session_op("SYSTEM", "MANAGER", FALP_ERASE, DEL, NULL, &auth, &uic, &xst, &sc, &sv);
        uint32_t ost = 0;
        int del_gone = (dnet_fal_ropen_st(DEL, &rf, NULL, NULL, &ost) != 0);
        if (!del_gone) (void)dnet_fal_rclose(rf);
        printf("  NOTE: SYSTEM DELETE -> client %08X, STATUS %04X STV %llX, server exit %08X;"
               " then $OPEN RMS %08X\n", (unsigned)st, (unsigned)sc, (unsigned long long)sv,
               (unsigned)xst, (unsigned)ost);
        FP_CHECK(st == SS$_NORMAL && del_gone && ost == RMS$_FNF,
                 "SYSTEM deletes its file through the FAL server process: RMS $OPEN then finds no such file (RMS-E-FNF)");
        (void)dnet_fal_erase(RENAMED, &s1, &s2);

        /* A DIRECTORY of a missing file in GUEST's SYS$LOGIN (live bracket
         * 2026-10-08: a VMS client printed "Total of 1 file" when the NAMEs
         * came back as volume + file with no directory). The miss must be
         * STATUS FNF, and any NAMEs must be the fully expanded spec. */
        st = falp_session_op("GUEST", "GUEST", FALP_DIRLIST, "SYS$LOGIN:NOSUCH_FALP.TXT;*", NULL,
                             &auth, &uic, &xst, &sc, &sv);
        printf("  NOTE: GUEST DIRECTORY SYS$LOGIN:NOSUCH_FALP.TXT;* -> client %08X, NAMEs '%s',"
               " STATUS %04X STV %llX\n", (unsigned)st, falp_names, (unsigned)sc,
               (unsigned long long)sv);
        int has_file = strstr(falp_names, "2:") != NULL, has_dir = strstr(falp_names, "4:") != NULL;
        FP_CHECK(st != SS$_NORMAL && sc == 0x4032 && sv == 0x0910 && (!has_file || has_dir),
                 "GUEST's remote DIRECTORY of a missing file is STATUS FNF 0x4032 STV 0x0910, never a file NAME without its directory (VMS prints NOFILES)");
    }

    printf("DECNETD-I-FALPROC, %d passed, %d failed\n", pass, fail);
    if (fail == 0 && pass > 0) { printf("DECNETD-FAL-PROC-ACCEPT: PASS\n"); return 0; }
    printf("DECNETD-FAL-PROC-ACCEPT: FAIL\n");
    return 1;
#undef FP_CHECK
}

/*
 * ================== --copy-selftest (rd vms-ea8/vms-6a4) ====================
 * The OUTBOUND $ COPY command layer that sits on top of the FAL client: the
 * node-filespec splitter + the copy-direction plan (dnet_copy_plan) that turn a
 * `COPY <src> <dst>` argument pair into {direction, node, creds, remote/local
 * spec}, then feed the object-17 connect builder + dnet_fal_client_get/put.
 *
 * This is the HONEST FLOOR (no executive, runs anywhere), the COPY analogue of
 * --fal-selftest: it proves (A) copy_plan derives the right direction + node +
 * access-control creds + node-stripped specs for a remote-SOURCE (GET) and a
 * remote-DEST (PUT) argument pair, and (B) the creds copy_plan parsed out of the
 * spec really drive a REAL object-17 Connect Initiate that FAL refuses with an
 * NSP disconnect when they cannot be authenticated (no /dev/vms here) -- for
 * BOTH directions, over the same threaded socketpair path DECNETD uses on the
 * live datalink. The AUTHENTICATED full GET/PUT transfer of a plan's specs is
 * the domain of --fal-accept-test (the hard gate on /dev/vms + the mounted
 * SYSUAF); this floor never fakes a transfer (INV-6).
 */
static int copy_plan_refused_without_auth(const struct dnet_copy_plan *plan,
                                           const char *label)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        fprintf(stderr, "DECNETD-E-COPYSELF, socketpair failed: %s\n", strerror(errno));
        return -1;
    }
    const uint8_t hwL[6] = { 0x02,0,0,0,0,0x0a };
    const uint8_t hwR[6] = { 0x02,0,0,0,0,0x0b };
    struct dnet_engine L, R;
    int rc = -1;
    if (dnet_engine_init(&L, 1, 10, "OVMXL", "EWA0", NULL, hwL, 0, 0, 0) == 0 &&
        dnet_engine_init(&R, 1, 11, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0) == 0) {
        dnet_tick_t tick = 100;
        uint32_t auth = 0;
        int br = fal_bringup(&L, &R, sv[0], sv[1], &tick,
                             plan->username, plan->password, &auth);
        if (br == 1 && auth != SS$_NORMAL) {
            printf("DECNETD-I-COPYSELF, %s: the creds copy_plan parsed drove a real"
                   " object-17 connect, REFUSED (status %08X) without auth (INV-6)\n",
                   label, auth);
            rc = 0;
        } else {
            printf("DECNETD-E-COPYSELF, %s: expected an honest refusal of the"
                   " unauthenticated connect, got bringup=%d auth=%08X\n",
                   label, br, auth);
            rc = 1;
        }
    } else {
        fprintf(stderr, "DECNETD-E-COPYSELF, engine init failed\n");
        rc = -1;
    }
    close(sv[0]); close(sv[1]);
    return rc;
}

static int run_copy_selftest(void)
{
    int pass = 0, fail = 0;
#define CP_CHECK(cond, msg) do { \
        if (cond) { printf("  PASS: %s\n", (msg)); pass++; } \
        else      { printf("  FAIL: %s\n", (msg)); fail++; } } while (0)

    /* (A) copy_plan direction/creds/spec derivation, GET and PUT. */
    struct dnet_copy_plan get_plan, put_plan;
    int rg = dnet_copy_plan("VAX1\"GUEST SECRET\"::DISK$U:[X]REMOTE.TXT",
                            "LOCAL.TXT", &get_plan);
    CP_CHECK(rg == 0 && get_plan.is_get == 1 &&
             !strcmp(get_plan.node, "VAX1") &&
             !strcmp(get_plan.username, "GUEST") &&
             !strcmp(get_plan.password, "SECRET") &&
             !strcmp(get_plan.remote_spec, "DISK$U:[X]REMOTE.TXT") &&
             !strcmp(get_plan.local_spec, "LOCAL.TXT"),
             "COPY remote-source -> GET plan (direction, node, creds, specs)");

    int rp = dnet_copy_plan("LOCAL.TXT",
                            "VAX1\"GUEST SECRET\"::DISK$U:[X]REMOTE.TXT", &put_plan);
    CP_CHECK(rp == 0 && put_plan.is_get == 0 &&
             !strcmp(put_plan.node, "VAX1") &&
             !strcmp(put_plan.remote_spec, "DISK$U:[X]REMOTE.TXT") &&
             !strcmp(put_plan.local_spec, "LOCAL.TXT"),
             "COPY remote-dest -> PUT plan (direction, node, specs)");

    /* refusals are structural, no wire needed */
    struct dnet_copy_plan tmp;
    CP_CHECK(dnet_copy_plan("A.TXT", "B.TXT", &tmp) == DNET_CTERM_EINVAL,
             "both-local COPY refused (not a DECnet transfer)");
    CP_CHECK(dnet_copy_plan("A::X", "B::Y", &tmp) == DNET_CTERM_EINVAL,
             "node-to-node COPY refused (not the outbound-client path)");

    /* (B) the parsed creds drive a real, honestly-refused object-17 connect,
     * for BOTH directions (integration: copy_plan -> connect builder -> engine). */
    if (rg == 0) {
        int r = copy_plan_refused_without_auth(&get_plan, "GET-plan creds");
        if (r < 0) return 1;
        CP_CHECK(r == 0, "GET-plan creds drive a real object-17 connect, refused without auth");
    }
    if (rp == 0) {
        int r = copy_plan_refused_without_auth(&put_plan, "PUT-plan creds");
        if (r < 0) return 1;
        CP_CHECK(r == 0, "PUT-plan creds drive a real object-17 connect, refused without auth");
    }

    printf("DECNETD-I-COPYSELF, %d passed, %d failed\n", pass, fail);
    if (fail == 0 && pass > 0) { printf("DECNETD-COPY-SELFTEST: PASS\n"); return 0; }
    printf("DECNETD-COPY-SELFTEST: FAIL\n");
    return 1;
#undef CP_CHECK
}

/* ============ outbound $ COPY client core (rd vms-ea8) =====================
 *
 * copy_client_run is the SINGLE outbound FAL (object 17) COPY client the DCL
 * COPY verb drives, over EITHER substrate:
 *   - a LIVE DECnet datalink (run_copy_loop: real AF_PACKET frames to a remote
 *     node's FAL, the lab-gated path, mirroring the --set-host client), or
 *   - a socketpair against a threaded OVMX FAL server (--copy-accept-test: the
 *     CI/battery-provable ground truth -- a real file moved BOTH directions
 *     through the SAME code path, byte-verified via RMS over the ACP).
 *
 * The substrate is the only difference, abstracted by struct copy_wire: txf()
 * ships one built link frame; rxev() pumps one inbound frame through the
 * engine's NSP link FSM and returns the resulting link event (leaving a
 * delivered DAP segment in eng->rx_data on DNET_LINK_EV_DATA). The DAP
 * presentation session itself is dnet_fal_client_get/put over a
 * dnet_dap_transport that rides copy_wire -- UNCHANGED from the FAL client
 * proven by --fal-accept-test; only the frame substrate differs. INV-6: no
 * fabricated transfer -- a missing peer, a failed connect-auth, or an absent RMS
 * volume all fail honestly.
 *
 * The datalink substrate (cw_dl_*) is thin glue over already-proven primitives
 * (scs_datalink_send + dnet_recv_route, the same the --set-host client and the
 * routing loop use); the novel logic (copy_client_run, the bring-up + DAP drive)
 * is the code --copy-accept-test exercises end to end.
 */
struct copy_wire {
    struct dnet_engine *eng;
    int (*txf)(struct copy_wire *w, const uint8_t *frame, size_t len);
    int (*rxev)(struct copy_wire *w, dnet_tick_t now); /* DNET_LINK_EV_* or -1 */
    int      sp_wfd, sp_rfd;      /* socketpair ends (test)     */
    int      dl_sock;            /* datalink fd (live)          */
    unsigned dl_if;              /* datalink ifindex (live)     */
    int      synthetic;          /* 1 => synthetic clock (test) */
    dnet_tick_t clk;             /* synthetic tick counter      */
};

static dnet_tick_t cw_now(struct copy_wire *w)
{
    return w->synthetic ? w->clk++ : monotonic_sec();
}

/* --- socketpair substrate (the --copy-accept-test ground truth) --- */
static int cw_sp_tx(struct copy_wire *w, const uint8_t *frame, size_t len)
{
    return write(w->sp_wfd, frame, len) == (ssize_t)len ? 0 : -1;
}
static int cw_sp_rx(struct copy_wire *w, dnet_tick_t now)
{
    uint8_t buf[DNET_FRAME_MAX], reply[DNET_FRAME_MAX];
    ssize_t n = read(w->sp_rfd, buf, sizeof buf);
    if (n <= 0) return -1;
    size_t rlen = 0; int has_reply = 0;
    enum dnet_link_event ev = DNET_LINK_EV_NONE;
    if (dnet_engine_link_rx(w->eng, now, buf, (size_t)n, reply, sizeof reply,
                            &rlen, &has_reply, &ev) != DNET_ENGINE_OK)
        return -1;
    if (has_reply && write(w->sp_wfd, reply, rlen) != (ssize_t)rlen) return -1;
    return (int)ev;
}

/* --- datalink substrate (the LIVE run_copy_loop path, lab-gated) --- */
static int cw_dl_tx(struct copy_wire *w, const uint8_t *frame, size_t len)
{
    uint8_t dst[DNET_ADDR_LEN];
    memcpy(dst, frame, DNET_ADDR_LEN);      /* the routing dst the FSM wrote */
    return scs_datalink_send(w->dl_sock, (int)w->dl_if, DNET_ETHERTYPE,
                             dst, frame, len) == 0 ? 0 : -1;
}
static int cw_dl_rx(struct copy_wire *w, dnet_tick_t now)
{
    uint8_t buf[DNET_FRAME_MAX];
    /* dnet_recv_route filters our own echo + foreign/HELLO frames, drives the
     * link FSM on an NSP frame for us, and ships any protocol reply itself. */
    return dnet_recv_route(w->eng, w->dl_sock, w->dl_if, now, buf, sizeof buf);
}

/* The DAP presentation transport over copy_wire (the copy_wire twin of the
 * fal_xport used by --fal-accept-test). */
struct copy_dap_ctx { struct copy_wire *w; };
static int copy_dap_send(void *ctx, const uint8_t *seg, size_t seglen)
{
    struct copy_wire *w = ((struct copy_dap_ctx *)ctx)->w;
    uint8_t frame[DNET_FRAME_MAX];
    size_t  flen = 0;
    if (dnet_engine_link_send(w->eng, seg, seglen, frame, sizeof frame, &flen,
                              cw_now(w)) != 0)
        return -1;
    return w->txf(w, frame, flen);
}
static int copy_dap_recv(void *ctx, uint8_t *buf, size_t cap, size_t *outlen)
{
    struct copy_wire *w = ((struct copy_dap_ctx *)ctx)->w;
    /* Bound a dead-peer hang on the LIVE datalink: cw_dl_rx returns NONE on each
     * SO_RCVTIMEO lapse, so a remote that stops answering mid-DAP must not loop
     * here forever. The socketpair test never yields NONE (it blocks or closes),
     * so this cap is invisible to it; it only fires honestly on a live stall. */
    int idle = 0;
    for (;;) {
        int ev = w->rxev(w, cw_now(w));
        if (ev < 0) return -1;
        if (ev == DNET_LINK_EV_DATA) {
            if (w->eng->rx_datalen > cap) return -1;
            memcpy(buf, w->eng->rx_data, w->eng->rx_datalen);
            *outlen = w->eng->rx_datalen;
            return 0;
        }
        if (ev == DNET_LINK_EV_DISCONNECT || ev == DNET_LINK_EV_DISCONNECT_CONF)
            return -1;
        if (ev == DNET_LINK_EV_NONE) {
            if (++idle > 60) return -1;   /* ~2 min at SO_RCVTIMEO=2s: give up honestly */
        } else {
            idle = 0;   /* ACK / LINK_SERVICE: real NSP traffic, the peer is alive */
        }
    }
}

/*
 * copy_client_run - open an object-17 FAL logical link to remote_area.node over
 * `w`, then drive dnet_fal_client_get/put per the plan. The access-control
 * username/account come from the plan; the PASSWORD is passed separately
 * (never on argv -- see run_copy_loop). Returns the FAL transfer status, or
 * SS$_ABORT if the link could not be established.
 */
static uint32_t copy_client_run(struct copy_wire *w,
                                unsigned remote_area, unsigned remote_node,
                                const struct dnet_copy_plan *plan,
                                const char *password)
{
    struct dnet_engine *eng = w->eng;
    uint8_t conn[192], frame[DNET_FRAME_MAX];
    size_t  clen = 0, flen = 0;

    /* The format-2 source group/user codes are the running process's own
     * identity (NONZERO), so real VMS session control dispatches the CI to the
     * FAL object instead of discarding it (vms-15a/a70). */
    uint16_t grp = 0, usr = 0;
    sethost_src_codes(&grp, &usr);
    if (dnet_cterm_sc_connect_build(DNET_OBJ_FAL, eng->node_name, grp, usr,
                                    plan->has_access ? plan->username : "",
                                    password ? password : "",
                                    plan->account, conn, sizeof conn, &clen) != 0)
        return SS$_ABORT;

    dnet_tick_t now = cw_now(w);
    if (dnet_engine_link_open(eng, remote_area, remote_node, 0x2001, conn, clen,
                              1459, 1, DNET_NSP_VER_41, frame, sizeof frame,
                              &flen, now) != DNET_ENGINE_OK)
        return SS$_ABORT;
    if (w->txf(w, frame, flen) != 0) return SS$_ABORT;

    /* Pump until the link is RUN (CC in), the object rejected us (DI in), or the
     * Connect-Initiate give-up fires (link CLOSED). Drive the CI retransmit /
     * give-up timers each iteration for the live datalink (no-op over the
     * lockstep socketpair, where the CC returns on the first read). */
    int up = 0;
    for (int i = 0; i < 4096 && !up; i++) {
        uint8_t tf[DNET_FRAME_MAX]; size_t tl = 0; int has = 0;
        if (dnet_engine_link_tick(eng, cw_now(w), tf, sizeof tf, &tl, &has)
                == DNET_ENGINE_OK && has)
            w->txf(w, tf, tl);
        int ev = w->rxev(w, cw_now(w));
        if (ev < 0) break;
        if (ev == DNET_LINK_EV_CONNECT_CONF && dnet_link_is_up(&eng->link)) { up = 1; break; }
        if (ev == DNET_LINK_EV_DISCONNECT || ev == DNET_LINK_EV_DISCONNECT_CONF) break;
        if (dnet_link_state_of(&eng->link) == DNET_LINK_CLOSED) break;
    }
    if (!up) return SS$_ABORT;

    /* NSP requires the INITIATOR to send a LINK SERVICE right after the CC (it
     * acks the CC + opens the flow-control window) before a real VAX will send
     * DAP; harmless to an OVMX FAL peer, which absorbs it (rd vms-6165). */
    now = cw_now(w);
    if (dnet_engine_link_service(eng, frame, sizeof frame, &flen, now)
            == DNET_ENGINE_OK)
        (void)w->txf(w, frame, flen);

    struct copy_dap_ctx dc = { w };
    struct dnet_dap_transport t = { .send = copy_dap_send, .recv = copy_dap_recv, .ctx = &dc };
    uint32_t status = plan->is_get
        ? dnet_fal_client_get(plan->remote_spec, plan->local_spec, &t)
        : dnet_fal_client_put(plan->local_spec, plan->remote_spec, &t);

    now = cw_now(w);
    if (dnet_engine_link_close(eng, DNET_LINK_REASON_NORMAL, frame, sizeof frame,
                               &flen, now) == DNET_ENGINE_OK)
        (void)w->txf(w, frame, flen);
    return status;
}

/*
 * copy_server_thread - the FAL (object 17) SERVER end of one --copy-accept-test
 * session: receive the object-17 Connect Initiate, AUTHENTICATE the carried
 * creds (dnet_fal_connect_auth -- the same SYSUAF/Purdy path LOGINOUT uses, no
 * file served on a bad connect, INV-6), accept -> Connect Confirm, then serve
 * the DAP session (dnet_fal_server_run) over the link. The exact server the
 * live datalink would drive, run in-thread so the client's copy_client_run is a
 * true black box.
 */
struct copy_server_arg { struct copy_wire w; uint32_t status; };
static void *copy_server_thread(void *v)
{
    struct copy_server_arg *a = v;
    struct copy_wire *w = &a->w;
    struct dnet_engine *eng = w->eng;
    uint8_t f[DNET_FRAME_MAX]; size_t fl = 0;
    a->status = SS$_ABORT;

    int got_ci = 0;
    for (int i = 0; i < 4096 && !got_ci; i++) {
        int ev = w->rxev(w, cw_now(w));
        if (ev < 0) return NULL;
        if (ev == DNET_LINK_EV_CONNECT_IND) got_ci = 1;
    }
    if (!got_ci) return NULL;

    char who[DNET_FAL_USER_MAX + 1];
    uint32_t auth = dnet_fal_connect_auth(eng->link.conn_data, eng->link.conn_len,
                                          who, sizeof who);
    if (auth != SS$_NORMAL) {
        if (dnet_engine_link_close(eng, DNET_LINK_REASON_OBJREJ, f, sizeof f, &fl,
                                   cw_now(w)) == DNET_ENGINE_OK)
            (void)w->txf(w, f, fl);
        a->status = auth;      /* honest refusal */
        return NULL;
    }
    if (dnet_engine_link_accept(eng, 0x2002, f, sizeof f, &fl, cw_now(w))
            != DNET_ENGINE_OK || w->txf(w, f, fl) != 0)
        return NULL;

    struct copy_dap_ctx dc = { w };
    struct dnet_dap_transport t = { .send = copy_dap_send, .recv = copy_dap_recv, .ctx = &dc };
    a->status = dnet_fal_server_run(&t);
    return NULL;
}

/* Run one COPY through copy_client_run against a threaded FAL server over a
 * socketpair datalink. Fills *client / *server with the two statuses. */
static int copy_xfer_once(const struct dnet_copy_plan *plan, const char *password,
                          uint32_t *client, uint32_t *server)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) return -1;
    const uint8_t hwL[6] = { 0x02,0,0,0,0,0x0a };
    const uint8_t hwR[6] = { 0x02,0,0,0,0,0x0b };
    static struct dnet_engine L, R;   /* static: large engine structs off-stack */
    if (dnet_engine_init(&L, 1, 10, "OVMXL", "EWA0", NULL, hwL, 0, 0, 0) != 0 ||
        dnet_engine_init(&R, 1, 11, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0) != 0) {
        close(sv[0]); close(sv[1]); return -1;
    }
    struct copy_server_arg sarg;
    memset(&sarg, 0, sizeof sarg);
    sarg.w.eng = &R; sarg.w.txf = cw_sp_tx; sarg.w.rxev = cw_sp_rx;
    sarg.w.sp_wfd = sv[1]; sarg.w.sp_rfd = sv[1]; sarg.w.synthetic = 1; sarg.w.clk = 1000;
    pthread_t th;
    if (pthread_create(&th, NULL, copy_server_thread, &sarg) != 0) {
        close(sv[0]); close(sv[1]); return -1;
    }
    struct copy_wire cw;
    memset(&cw, 0, sizeof cw);
    cw.eng = &L; cw.txf = cw_sp_tx; cw.rxev = cw_sp_rx;
    cw.sp_wfd = sv[0]; cw.sp_rfd = sv[0]; cw.synthetic = 1; cw.clk = 100;
    uint32_t cs = copy_client_run(&cw, 1, 11, plan, password);
    pthread_join(th, NULL);
    close(sv[0]); close(sv[1]);
    if (client) *client = cs;
    if (server) *server = sarg.status;
    return 0;
}

/*
 * run_copy_accept_test (rd vms-ea8) -- the BATTERY proof of the outbound $ COPY
 * command layer: a real COPY argument pair is parsed by dnet_copy_plan (the same
 * parser the DCL COPY verb uses) and driven through copy_client_run to an
 * authenticated object-17 FAL server over an NSP link, moving a sequential file
 * BOTH directions with the records byte-verified through real RMS over the ACP.
 * The FAL analogue of --fal-accept-test, but entered through the COPY command's
 * own plan rather than hardcoded specs. Needs /dev/vms + the mounted SYSUAF
 * (GUEST/GUEST); it FAILS honestly where the executive is absent (INV-6).
 */
static int run_copy_accept_test(void)
{
    printf("DECNETD-I-COPYACCEPT, outbound $ COPY command layer (dnet_copy_plan)"
           " -> object-17 FAL client over the NSP link -> real RMS transfer both"
           " directions, byte-verified (rd vms-ea8)\n");
    int pass = 0, fail = 0;
#define CA_CHECK(c, msg) do { if (c) { pass++; printf("  PASS: %s\n", msg); } \
    else { fail++; printf("  FAIL: %s\n", msg); } } while (0)

    static const char *lines[] = {
        "COPY line one over the outbound DCL client (vms-ea8)",
        "COPY line two - a multi-record DAP data transfer",
        "COPY line three and final"
    };
    const int nl = 3;
    const char *SRC  = "SYS$SYSROOT:[SYSMGR]OVMXCOPY_S.TXT";
    const char *DEST = "SYS$SYSROOT:[SYSMGR]OVMXCOPY_D.TXT";
    const char *BACK = "SYS$SYSROOT:[SYSMGR]OVMXCOPY_B.TXT";

    int src_ok = (rms_textfile_write_line(SRC, lines[0]) == 0) &&
                 (rms_textfile_append_line(SRC, lines[1]) == 0) &&
                 (rms_textfile_append_line(SRC, lines[2]) == 0);
    CA_CHECK(src_ok, "source file created on the ODS-2 volume via RMS over the ACP");

    /* PUT: $ COPY SRC OVMXR"GUEST GUEST"::DEST -> plan(is_get=0) -> copy_client_run. */
    if (src_ok) {
        char dstspec[320];
        snprintf(dstspec, sizeof dstspec, "OVMXR\"GUEST GUEST\"::%s", DEST);
        struct dnet_copy_plan plan;
        int rp = dnet_copy_plan(SRC, dstspec, &plan);
        CA_CHECK(rp == 0 && plan.is_get == 0 && !strcmp(plan.node, "OVMXR"),
                 "COPY local->remote parses to a PUT plan (node OVMXR)");
        if (rp == 0) {
            uint32_t cs = 0, ss = 0;
            if (copy_xfer_once(&plan, plan.password, &cs, &ss) == 0) {
                CA_CHECK(cs == SS$_NORMAL && ss == SS$_NORMAL,
                         "PUT: the COPY plan drove a full DAP transfer, both peers OK");
                CA_CHECK(fal_file_matches(DEST, lines, nl),
                         "PUT: the STORED file's records BYTE-MATCH the source (real transfer)");
            } else { fail++; printf("  FAIL: PUT transfer harness setup\n"); }
        }
    }

    /* GET: $ COPY OVMXR"GUEST GUEST"::DEST BACK -> plan(is_get=1) -> copy_client_run. */
    {
        char srcspec[320];
        snprintf(srcspec, sizeof srcspec, "OVMXR\"GUEST GUEST\"::%s", DEST);
        struct dnet_copy_plan plan;
        int rg = dnet_copy_plan(srcspec, BACK, &plan);
        CA_CHECK(rg == 0 && plan.is_get == 1 && !strcmp(plan.node, "OVMXR"),
                 "COPY remote->local parses to a GET plan (node OVMXR)");
        if (rg == 0) {
            uint32_t cs = 0, ss = 0;
            if (copy_xfer_once(&plan, plan.password, &cs, &ss) == 0) {
                CA_CHECK(cs == SS$_NORMAL && ss == SS$_NORMAL,
                         "GET: the COPY plan drove a full DAP transfer, both peers OK");
                CA_CHECK(fal_file_matches(BACK, lines, nl),
                         "GET: the FETCHED file's records BYTE-MATCH the source (real transfer)");
            } else { fail++; printf("  FAIL: GET transfer harness setup\n"); }
        }
    }

    printf("DECNETD-I-COPYACCEPT, %d passed, %d failed\n", pass, fail);
    if (fail == 0 && pass > 0) { printf("DECNETD-COPY-ACCEPT: PASS\n"); return 0; }
    printf("DECNETD-COPY-ACCEPT: FAIL\n");
    return 1;
#undef CA_CHECK
}

/*
 * copy_noauth_server_thread - a TEST-DOUBLE FAL peer for the host transport
 * proof: it accepts the object-17 connect WITHOUT authenticating (this half
 * proves copy_client_run's bring-up + link-service + DAP transport, NOT auth --
 * the auth gate is proven separately by --copy-selftest's refused-without-auth
 * assertions and end to end by --copy-accept-test on a real SYSUAF). It receives
 * the CI, accepts -> Connect Confirm, then serves the DAP session; a GET of a
 * file it cannot open (no ACP volume on the build host) yields the honest
 * STATUS(access-failed) that must round-trip back over the NSP link.
 */
static void *copy_noauth_server_thread(void *v)
{
    struct copy_server_arg *a = v;
    struct copy_wire *w = &a->w;
    struct dnet_engine *eng = w->eng;
    uint8_t f[DNET_FRAME_MAX]; size_t fl = 0;
    a->status = SS$_ABORT;

    int got_ci = 0;
    for (int i = 0; i < 4096 && !got_ci; i++) {
        int ev = w->rxev(w, cw_now(w));
        if (ev < 0) return NULL;
        if (ev == DNET_LINK_EV_CONNECT_IND) got_ci = 1;
    }
    if (!got_ci) return NULL;
    if (dnet_engine_link_accept(eng, 0x2002, f, sizeof f, &fl, cw_now(w))
            != DNET_ENGINE_OK || w->txf(w, f, fl) != 0)
        return NULL;

    struct copy_dap_ctx dc = { w };
    struct dnet_dap_transport t = { .send = copy_dap_send, .recv = copy_dap_recv, .ctx = &dc };
    a->status = dnet_fal_server_run(&t);
    return NULL;
}

/*
 * run_copy_transport_selftest (rd vms-ea8) -- the HOST FLOOR for the outbound
 * COPY client: proves copy_client_run itself (client-only bring-up over the
 * datalink substrate: Connect Initiate -> await Connect Confirm -> NSP link
 * service -> DAP session -> disconnect) drives a full DAP config + access +
 * honest STATUS round-trip end to end over the copy_wire transport, against a
 * threaded test-double FAL server, with NO executive and NO CAP_NET_RAW. A GET
 * of a file the server cannot open must return the honest miss on BOTH peers --
 * exactly the --fal-selftest transport-pump discipline, but through the SAME
 * copy_client_run the live $ COPY uses. The authenticated byte-verified transfer
 * is --copy-accept-test (needs /dev/vms + SYSUAF).
 */
static int run_copy_transport_selftest(void)
{
    printf("DECNETD-I-COPYXPORT, outbound COPY client transport pump: copy_client_run"
           " brings up an object-17 link + DAP session over the NSP link, honest miss"
           " round-trips (no executive, rd vms-ea8)\n");
    int pass = 0, fail = 0;
#define CX_CHECK(c, msg) do { if (c) { pass++; printf("  PASS: %s\n", msg); } \
    else { fail++; printf("  FAIL: %s\n", msg); } } while (0)

    struct dnet_copy_plan plan;
    int rp = dnet_copy_plan("OVMXR::DKA0:[X]NOPE.TXT", "DKA0:[X]LOCAL.TXT", &plan);
    CX_CHECK(rp == 0 && plan.is_get == 1, "COPY remote->local parses to a GET plan");
    if (rp != 0) { printf("DECNETD-COPY-XPORT: FAIL\n"); return 1; }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        fprintf(stderr, "DECNETD-E-COPYXPORT, socketpair failed: %s\n", strerror(errno));
        return 1;
    }
    const uint8_t hwL[6] = { 0x02,0,0,0,0,0x0a };
    const uint8_t hwR[6] = { 0x02,0,0,0,0,0x0b };
    static struct dnet_engine L, R;
    int setup_ok = (dnet_engine_init(&L, 1, 10, "OVMXL", "EWA0", NULL, hwL, 0, 0, 0) == 0 &&
                    dnet_engine_init(&R, 1, 11, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0) == 0);
    CX_CHECK(setup_ok, "two engines initialised");
    if (setup_ok) {
        struct copy_server_arg sarg;
        memset(&sarg, 0, sizeof sarg);
        sarg.w.eng = &R; sarg.w.txf = cw_sp_tx; sarg.w.rxev = cw_sp_rx;
        sarg.w.sp_wfd = sv[1]; sarg.w.sp_rfd = sv[1]; sarg.w.synthetic = 1; sarg.w.clk = 1000;
        pthread_t th;
        if (pthread_create(&th, NULL, copy_noauth_server_thread, &sarg) == 0) {
            struct copy_wire cw;
            memset(&cw, 0, sizeof cw);
            cw.eng = &L; cw.txf = cw_sp_tx; cw.rxev = cw_sp_rx;
            cw.sp_wfd = sv[0]; cw.sp_rfd = sv[0]; cw.synthetic = 1; cw.clk = 100;
            uint32_t cs = copy_client_run(&cw, 1, 11, &plan, "");
            pthread_join(th, NULL);
            CX_CHECK(cs == SS$_NOSUCHFILE && sarg.status == SS$_NOSUCHFILE,
                     "copy_client_run brought the link up + drove the DAP session; the"
                     " honest miss round-trips end to end (client + server both NOSUCHFILE)");
        } else { fail++; printf("  FAIL: server thread create\n"); }
    }
    close(sv[0]); close(sv[1]);

    printf("DECNETD-I-COPYXPORT, %d passed, %d failed\n", pass, fail);
    if (fail == 0 && pass > 0) { printf("DECNETD-COPY-XPORT: PASS\n"); return 0; }
    printf("DECNETD-COPY-XPORT: FAIL\n");
    return 1;
#undef CX_CHECK
}

/*
 * run_copy_loop - the LIVE outbound $ COPY over the datalink (rd vms-ea8).
 *
 * Parses the copy plan, ENFORCES the credential posture (below), resolves the
 * remote node, then drives copy_client_run over the real datalink `sock`. The
 * transfer core is the code --copy-accept-test proves byte-exact; the only
 * lab-gated delta is reaching a real remote FAL over AF_PACKET (mirrors the
 * --set-host client, rd vms-a70 / vms-101).
 *
 * CREDENTIAL POSTURE (a DECIDED rule, not a facade): the FAL access-control
 * PASSWORD is a REAL credential (unlike SET HOST, where LOGINOUT authenticates
 * fresh and the connect password is empty). It MUST NOT appear on argv -- argv
 * is world-readable in /proc/<pid>/cmdline under a fork/exec activation -- so a
 * password embedded in the COPY spec's access string is REFUSED here (the
 * command line carries at most NODE"username"::spec), and the password is read
 * from the inherited fd named by --password-fd, never crossing a process
 * boundary in the clear.
 */
/* Parse the COPY plan, ENFORCE the credential posture, and read the password
 * from the inherited fd (never argv). Shared by the standalone datalink client
 * and the NETACP-brokered _NET: client. password must hold DNET_SC_MAX_STR+1. */
static int copy_prepare(const char *src, const char *dst, int password_fd,
                        struct dnet_copy_plan *plan, char *password)
{
    memset(password, 0, DNET_SC_MAX_STR + 1);
    int r = dnet_copy_plan(src, dst, plan);
    if (r != DNET_CTERM_OK) {
        fprintf(stderr, "DECNETD-E-COPYSPEC, could not parse the COPY specs"
                        " (one side must be NODE\"user\"::file; status %d)\n", r);
        return 1;
    }

    /* POSTURE: never accept the password on the command line. */
    if (plan->password[0] != '\0') {
        fprintf(stderr, "DECNETD-E-COPYPW, the FAL password must not appear on the"
                        " command line (it would be world-readable in"
                        " /proc/<pid>/cmdline); pass NODE\"username\"::file and"
                        " supply the password on the fd named by --password-fd\n");
        return 1;
    }
    if (plan->has_access && password_fd < 0) {
        fprintf(stderr, "DECNETD-E-COPYPW, an access-control username was given"
                        " but no --password-fd; refusing (no password source)\n");
        return 1;
    }

    /* Read the password (if any) from the inherited fd, never from argv. Bounded;
     * a trailing newline is stripped; the buffer is wiped after the transfer.
     *
     * CLEARTEXT TRANSMISSION (codeql cpp/cleartext-transmission, BY DESIGN):
     * this password IS carried to the remote FAL in the object-17 Session Control
     * CONNECT (copy_client_run -> dnet_cterm_sc_connect_build), where FAL
     * authenticates it -- that is how DECnet Phase IV FAL access control works
     * (the oracle §1 shows the credentials in the connect; the OPPOSITE of CTERM,
     * whose connect creds are empty). DECnet Phase IV has NO transport encryption;
     * OVMX is a CLEAN-ROOM FAITHFUL reproduction (Rule 8) and cannot encrypt what
     * the wire protocol defines as cleartext -- the same property the shipped
     * inbound FAL server (decnet$fal, --fal-accept-test) already has. OVMX's own
     * hardening is orthogonal and present: the password never touches argv (fd
     * handoff), is bounded, and is wiped immediately after the connect is built. */
    if (plan->has_access && password_fd >= 0) {
        ssize_t got = read(password_fd, password, DNET_SC_MAX_STR); // codeql[cpp/cleartext-transmission]
        if (got < 0) {
            fprintf(stderr, "DECNETD-E-COPYPW, could not read the password from"
                            " fd %d: %s\n", password_fd, strerror(errno));
            return 1;
        }
        password[got >= 0 ? (size_t)got : 0] = '\0';
        size_t pl = strlen(password);
        while (pl && (password[pl - 1] == '\n' || password[pl - 1] == '\r'))
            password[--pl] = '\0';
    }

    return 0;
}

static int run_copy_loop(struct dnet_engine *eng, int sock, unsigned ifindex,
                         const char *src, const char *dst, int password_fd)
{
    struct dnet_copy_plan plan;
    char password[DNET_SC_MAX_STR + 1];
    if (copy_prepare(src, dst, password_fd, &plan, password) != 0)
        return 1;

    /* Resolve the remote node NAME -> area.node (the node database the SET HOST
     * client uses too); no address is ever invented (INV-6). */
    unsigned rarea = 0, rnode = 0;
    if (sethost_resolve_target(plan.node, &rarea, &rnode) != 0) {
        fprintf(stderr, "DECNETD-E-NOSUCHNODE, COPY: cannot resolve node '%s'"
                        " (not area.node, and not a NAME in the node database)\n",
                plan.node);
        return 1;
    }

    /* A process context is required for the local file's RMS channels, exactly
     * as the --set-host client establishes one before $ASSIGN. */
    if (!vms_pcb_get())
        vms_pcb_init(0);

    /* Bound the datalink recv so the synchronous DAP pump can drive CI retransmit
     * / give-up timers rather than block forever waiting on an unreachable peer. */
    struct timeval rcv_to = { 2, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof rcv_to);

    log_ts(stdout);
    printf(" DECNETD-I-COPYPLAN, %s %s%s%s::%s <-> local %s (node %u.%u)\n",
           plan.is_get ? "GET" : "PUT",
           plan.node,
           plan.has_access ? "\"" : "", plan.has_access ? plan.username : "",
           plan.remote_spec, plan.local_spec, rarea, rnode);
    fflush(stdout);

    struct copy_wire w;
    memset(&w, 0, sizeof w);
    w.eng = eng; w.txf = cw_dl_tx; w.rxev = cw_dl_rx;
    w.dl_sock = sock; w.dl_if = ifindex; w.synthetic = 0;

    uint32_t status = copy_client_run(&w, rarea, rnode, &plan, password);
    memset(password, 0, sizeof password);

    log_ts(stdout);
    if (status == SS$_NORMAL) {
        printf(" DECNETD-I-COPYDONE, $ COPY %s completed (%08X)\n",
               plan.is_get ? "GET" : "PUT", status);
        fflush(stdout);
        return 0;
    }
    printf(" DECNETD-W-COPYFAIL, $ COPY %s did not complete (status %08X) --"
           " no file transferred (INV-6)\n", plan.is_get ? "GET" : "PUT", status);
    fflush(stdout);
    return 1;
}

/*
 * ================== THE _NET: CLIENT (rd vms-dda) ==================
 *
 * On a booted node the DCL COPY / SET HOST clients do NOT run a DECnet engine
 * of their own: NETACP owns the node's NSP address and every logical link, so a
 * second engine would race it for every inbound segment (and the executive
 * refuses it the datalink). The client does what a VMS image does instead --
 * $ASSIGN _NET:, then $QIO IO$_ACCESS (an NCB naming node + object, with the
 * access control), IO$_WRITEVBLK / IO$_READVBLK, IO$_DEACCESS -- and libvms's
 * qio_net_op brokers each of those to NETACP (dnet_broker_xfer over the
 * executive mailboxes).
 *
 * struct netcli is that channel. In the host-floor selftest the same calls run
 * dnet_broker_xfer -- the code qio_net_op runs -- over an in-process queue to a
 * real NETACP pool instead of over the executive mailboxes.
 */
struct netcli {
    uint16_t chan;                          /* live: the $ASSIGN _NET: channel */
    struct dnet_broker_chan *bc;            /* selftest: in-process channel ... */
    const struct dnet_broker_io *io;        /* ... and its transport           */
};

/* Polls (each followed by an idle) a DAP/CTERM read waits on a silent link
 * before giving up: ~2 min at the live 5 ms idle, the bound the standalone
 * datalink client used. */
#define NETCLI_RECV_IDLE_POLLS 24000u

static uint32_t netcli_op(struct netcli *c, uint16_t op, const void *in, size_t inlen,
                          void *out, size_t outcap, size_t *xfer)
{
    if (xfer) *xfer = 0;
    if (c->io)
        return dnet_broker_xfer(c->bc, c->io, op, in, inlen, out, outcap, xfer);

    uint32_t func = 0;
    void *p1 = NULL;
    uint32_t p2 = 0;
    switch (op & DNET_BROKER_OP_MASK) {
    case DNET_BROKER_OP_OPEN:  func = IO$_ACCESS;    p1 = (void *)in; p2 = (uint32_t)inlen; break;
    case DNET_BROKER_OP_SEND:  func = IO$_WRITEVBLK; p1 = (void *)in; p2 = (uint32_t)inlen; break;
    case DNET_BROKER_OP_RECV:
        func = IO$_READVBLK; p1 = out; p2 = (uint32_t)outcap;
        if (op & DNET_BROKER_OPF_NOW)
            func |= IO$M_NOW;
        break;
    case DNET_BROKER_OP_CLOSE: func = IO$_DEACCESS;  break;
    default: return SS$_ILLIOFUNC;
    }
    /* How the I/O ENDED, not whether it was queued (dnet_netqio.h, rd vms-d01):
     * reading only $QIOW's service status took a refused connect for an open
     * link -- a bad-password COPY then polled a link that never existed. */
    return dnet_net_qiow(c->chan, func, p1, p2, xfer);
}

static void netcli_idle(struct netcli *c)
{
    if (c->io) {
        if (c->io->idle) c->io->idle(c->io->ctx);
        return;
    }
    struct timespec ts = { 0, 5 * 1000 * 1000 };
    nanosleep(&ts, NULL);
}

/* The DAP presentation transport over a _NET: link (COPY's FAL client). */
static int netcli_dap_send(void *ctx, const uint8_t *seg, size_t seglen)
{
    size_t x = 0;
    return (netcli_op(ctx, DNET_BROKER_OP_SEND, seg, seglen, NULL, 0, &x) & 1) ? 0 : -1;
}
static int netcli_dap_recv(void *ctx, uint8_t *buf, size_t cap, size_t *outlen)
{
    for (unsigned i = 0;; i++) {
        size_t x = 0;
        uint32_t st = netcli_op(ctx, DNET_BROKER_OP_RECV | DNET_BROKER_OPF_NOW, NULL, 0,
                                buf, cap, &x);
        if (st == SS$_ENDOFFILE) {
            if (i >= NETCLI_RECV_IDLE_POLLS)
                return -1;                 /* a silent link: give up honestly */
            netcli_idle(ctx);
            continue;
        }
        if (!(st & 1))
            return -1;                     /* link aborted / disconnected */
        *outlen = x;
        return 0;
    }
}

/*
 * copy_client_run_net - one $ COPY over a NETACP-brokered link: IO$_ACCESS to
 * the FAL object (17) with the plan's access control in the NCB, the DAP
 * session, IO$_DEACCESS. Returns the FAL transfer status, or the IO$_ACCESS
 * failure (SS$_INVLOGIN when the remote refused the credentials, ...).
 */
static uint32_t copy_client_run_net(struct netcli *c, const struct dnet_copy_plan *plan,
                                    const char *password)
{
    char ncb[DNET_NSP_MAX_DATA];
    const char *pw = password ? password : "";
    /* The NCB access string is blank-separated and quoted: a field that holds a
     * blank or a quote cannot be carried -- refuse rather than mis-split it. */
    if (strpbrk(plan->username, " \"") || strpbrk(pw, " \"") ||
        strpbrk(plan->account, " \"") || (plan->account[0] && !pw[0]))
        return SS$_BADPARAM;
    int n;
    if (plan->has_access)
        n = snprintf(ncb, sizeof ncb, "%s\"%s%s%s%s%s\"::\"%d=\"", plan->node,
                     plan->username, pw[0] ? " " : "", pw,
                     plan->account[0] ? " " : "", plan->account, DNET_FAL_OBJECT);
    else
        n = snprintf(ncb, sizeof ncb, "%s::\"%d=\"", plan->node, DNET_FAL_OBJECT);
    if (n < 0 || (size_t)n >= sizeof ncb) {
        memset(ncb, 0, sizeof ncb);
        return SS$_BADPARAM;
    }
    size_t x = 0;
    uint32_t st = netcli_op(c, DNET_BROKER_OP_OPEN, ncb, (size_t)n, NULL, 0, &x);
    memset(ncb, 0, sizeof ncb);            /* it carried the password */
    if (!(st & 1))
        return st;

    struct dnet_dap_transport t = { .send = netcli_dap_send, .recv = netcli_dap_recv,
                                    .ctx = c };
    uint32_t status = plan->is_get
        ? dnet_fal_client_get(plan->remote_spec, plan->local_spec, &t)
        : dnet_fal_client_put(plan->local_spec, plan->remote_spec, &t);
    (void)netcli_op(c, DNET_BROKER_OP_CLOSE, NULL, 0, NULL, 0, &x);
    return status;
}

/* Is a NETACP serving logical links on this node? (Its request mailbox is
 * published as DNET$NETACP_REQ.) 1 = yes, 0 = no, -1 = no executive at all. */
#define NETACP_REQ_LOGNAM "DNET$NETACP_REQ"
static int netacp_running(void)
{
    char dev[64];
    uint16_t dl = 0;
    int r = vms_kif_lnm_translate(VMS_LNM_TBL_SYSTEM, NETACP_REQ_LOGNAM, 0, dev,
                                  sizeof dev - 1, &dl, NULL, NULL);
    if (r < 0) return -1;
    return (r == 1 && dl > 0) ? 1 : 0;
}

static int netcli_assign(struct netcli *c)
{
    memset(c, 0, sizeof *c);
    if (!vms_pcb_get())
        vms_pcb_init(0);
    struct dsc$descriptor_s d;
    sethost_mkdesc(&d, "_NET:");
    uint32_t st = sys$assign(&d, &c->chan, 0, NULL);
    if (!(st & 1)) {
        fprintf(stderr, "DECNETD-E-NONET, $ASSIGN _NET: failed (status %08X) -- no"
                        " DECnet device on this node\n", (unsigned)st);
        return -1;
    }
    return 0;
}

/* $ COPY through NETACP (the booted runtime). */
static int run_copy_net(const char *src, const char *dst, int password_fd)
{
    struct dnet_copy_plan plan;
    char password[DNET_SC_MAX_STR + 1];
    if (copy_prepare(src, dst, password_fd, &plan, password) != 0)
        return 1;
    struct netcli c;
    if (netcli_assign(&c) != 0) {
        memset(password, 0, sizeof password);
        return 1;
    }
    /* VMS COPY is SILENT on success; on failure it names the file and the
     * reason (%COPY-E-OPENIN/OPENOUT, then the status, as a continuation
     * line). The DECNETD-I progress chatter belongs to the standalone/lab
     * path, never to a user's DCL COPY. */
    uint32_t status = copy_client_run_net(&c, &plan, password);
    memset(password, 0, sizeof password);
    sys$dassgn(c.chan);
    if (status == SS$_NORMAL)
        return 0;
    char why[256] = "";
    (void)vms_status_string(status, why, sizeof why);
    if (why[0] == '%') why[0] = '-';
    if (plan.is_get)
        printf("%%COPY-E-OPENIN, error opening %s::%s as input\n%s\n",
               plan.node, plan.remote_spec, why[0] ? why : "");
    else
        printf("%%COPY-E-OPENOUT, error opening %s::%s as output\n%s\n"
               "%%COPY-W-NOTCOPIED, %s not copied\n",
               plan.node, plan.remote_spec, why[0] ? why : "", plan.local_spec);
    fflush(stdout);
    return 1;
}

/*
 * $ SET HOST through NETACP (the booted runtime): IO$_ACCESS to the CTERM
 * object (42) on `target`, then the same CTERM client handlers as the
 * standalone path, reading the link with IO$_READVBLK|IO$M_NOW between polls of
 * the local terminal. The connect's source identity is this process's, filled
 * in by NETACP from the executive (so --user does not apply here); the REMOTE
 * LOGINOUT authenticates fresh either way.
 */
static int run_set_host_net(struct netcli *c, const char *target, const char *local_node)
{
    static struct sh_ctx x;
    if (sethost_ctx_init(&x) != 0)
        return 1;
    x.nc = c;
    x.where = "a NETACP-brokered _NET: link";
    x.quiet = 1;                        /* VMS SET HOST shows only the remote */
    if (sethost_open_terminal(&x) != 0)
        return 1;
    int term_fd = vms$$chan_to_fd(x.ch_in);

    char ncb[64];
    int n = snprintf(ncb, sizeof ncb, "%s::\"%d=\"", target, DNET_CTERM_OBJECT);
    size_t xf = 0;
    uint32_t st = (n > 0 && (size_t)n < sizeof ncb)
        ? netcli_op(c, DNET_BROKER_OP_OPEN, ncb, (size_t)n, NULL, 0, &xf) : SS$_BADPARAM;
    if (!(st & 1)) {
        /* VMS SET HOST reports only the reason the link failed. */
        char why[256] = "";
        (void)vms_status_string(st, why, sizeof why);
        printf("%s\n", why[0] ? why : "%SYSTEM-F-ABORT, abort");
        fflush(stdout);
        sys$dassgn(x.ch_in); sys$dassgn(x.ch_out);
        return 1;
    }
    sethost_on_linkup(&x);

    while (!g_stop && !x.done) {
        for (int k = 0; k < 64 && !x.done; k++) {
            uint8_t buf[DNET_NSP_MAX_DATA];
            size_t got = 0;
            st = netcli_op(c, DNET_BROKER_OP_RECV | DNET_BROKER_OPF_NOW, NULL, 0,
                           buf, sizeof buf, &got);
            if (st == SS$_ENDOFFILE)
                break;
            if (!(st & 1)) {
                x.done = 1;
                break;
            }
            sethost_on_data(&x, buf, got);
        }
        if (x.done)
            break;
        if (dnet_cterm_is_bound(&x.term) && !x.stdin_eof && term_fd >= 0) {
            struct pollfd pfd = { term_fd, POLLIN, 0 };
            int pr = poll(&pfd, 1, 20);
            if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP)))
                sethost_on_term_input(&x);
        } else {
            struct timespec ts = { 0, 20 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    sethost_finish(&x, local_node);
    (void)netcli_op(c, DNET_BROKER_OP_CLOSE, NULL, 0, NULL, 0, &xf);
    sys$dassgn(x.ch_in);
    sys$dassgn(x.ch_out);
    return x.rc;
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
        "  --iface IFNAME      datalink interface. If omitted, the primary NIC is\n"
        "                      AUTO-DETECTED (first up, non-loopback L2 interface --\n"
        "                      the one the executive's _NET: rides), so the daemon\n"
        "                      STARTNET.COM runs with no argv binds the right NIC;\n"
        "                      falls back to %s only if detection finds nothing\n"
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
        "  --task-selftest     run the TASK-TO-TASK client floor and exit (no\n"
        "                      executive): open a link by NAME (NODE::\"TASK=x\",\n"
        "                      format-1 NAMED descriptor), the passive side decodes\n"
        "                      the name, a request+reply move BOTH directions\n"
        "                      byte-verified, then disconnect (rd vms-dda)\n"
        "  --net-broker-selftest  run the exec<->NETACP T1 broker record codec\n"
        "                      floor and exit (no executive): request/response\n"
        "                      round-trip + bounds-validated decode (fuzzed) +\n"
        "                      correlation-id anti-cross-talk (rd vms-22c)\n"
        "  --net-service-selftest run the NETACP broker service-dispatch floor and\n"
        "                      exit (no executive): OPEN/SEND/RECV/CLOSE against the\n"
        "                      NSP link engine over a socketpair, correlation-matched,\n"
        "                      with honest never-crash refusals (rd vms-22c)\n"
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
        "                      through real DAP + RMS over the ACP, byte-verified\n"
        "  --netacp-pool-selftest  run NETACP's inbound session-pool proof (booted\n"
        "                      executive): concurrent SET HOST sessions, per-node\n"
        "                      cap, bounded pool, FAL dispatch (rd vms-6af1)\n"
        "  --mail11-accept-test  run the inbound DECnet MAIL-11 proof (booted\n"
        "                      executive): the real VAX capture replayed through\n"
        "                      NETACP to MAIL_SERVER.EXE, replies byte-exact;\n"
        "                      the message is stored in SYSTEM's mail (vms-47fd)\n"
        "  --netacp-show-selftest [--oracle-dir D]  run the NCP SHOW / SHOW NETWORK\n"
        "                      floor (snapshot codec, NETACP serving from live state,\n"
        "                      oracle layout vs docs/oracle/vax-ncp-show) and exit\n"
        "  --netacp-broker-selftest  run the outbound-links-through-NETACP floor and\n"
        "                      exit (no executive): _NET: broker records -> NETACP\n"
        "                      pool -> NSP link; COPY, refusals, bounds (vms-dda)\n"
        "  --net-loopback-accept-test  run the booted $QIO _NET: proof: COPY 0::\n"
        "                      through NETACP's loopback to FAL.EXE (vms-dda)\n"
        "  --fal-proc-accept-test  run the FAL server-PROCESS persona proof (booted\n"
        "                      executive): FAL.EXE runs with the authenticated\n"
        "                      user's UIC; GUEST is refused a SYSTEM-only file\n"
        "                      that SYSTEM reads through the same path (vms-d85)\n"
        "  --copy-selftest     run the OUTBOUND COPY command-layer floor and exit\n"
        "                      (no executive): copy_plan derives the right\n"
        "                      direction + node + creds + specs for a remote-source\n"
        "                      (GET) and remote-dest (PUT) pair, and those parsed\n"
        "                      creds drive a real object-17 connect refused without\n"
        "                      auth -- both directions (rd vms-ea8/vms-6a4)\n"
        "  --copy-transport-selftest  run the outbound COPY client PUMP floor and\n"
        "                      exit (no executive): copy_client_run brings up an\n"
        "                      object-17 link + NSP link-service + DAP session over\n"
        "                      the copy_wire transport against a test-double server,\n"
        "                      an honest miss round-trips both peers (rd vms-ea8)\n"
        "  --copy-accept-test  run the FULL outbound COPY transfer proof and exit\n"
        "                      (a HARD GATE on /dev/vms + the mounted SYSUAF): a\n"
        "                      COPY argument pair is parsed by copy_plan and driven\n"
        "                      through the object-17 FAL client over an NSP link to\n"
        "                      a threaded FAL server, moving a sequential file BOTH\n"
        "                      directions, records byte-verified via RMS (vms-ea8)\n"
        "  --copy SRC DST      OUTBOUND $ COPY over DECnet: exactly one of SRC/DST\n"
        "                      is NODE\"username\"::file (the remote), the other is\n"
        "                      local. The FAL PASSWORD is NEVER taken here -- it is\n"
        "                      a real credential and must not sit in argv (world-\n"
        "                      readable /proc); supply it on --password-fd. The\n"
        "                      outbound datalink transport is build-host-gated.\n"
        "  --password-fd N     with --copy: read the FAL access-control password\n"
        "                      from inherited fd N (never from argv or the env)\n",
        argv0, DECNETD_DEFAULT_IFACE, (unsigned)DNET_T3_DEFAULT,
        (unsigned)DNET_ROUTER_PRIORITY_DEFAULT);
}

/*
 * ================ NETACP INBOUND SESSION POOL (rd vms-6af1, vms-d85) ================
 *
 * WHY A POOL. The first inbound rung served ONE session at a time over the
 * engine's single logical link: an unauthenticated peer that connected and
 * stalled at LOGINOUT's "Username:" (then reconnected the moment LOGINOUT's
 * login read timed out) could hold the only slot continuously and deny every
 * legitimate inbound SET HOST (docs/security/decnet-networking-r4-sweep.md G2).
 * Real VMS serves many concurrent network sessions, bounded by the executor's
 * MAXIMUM LINKS. NETACP now keeps a bounded pool of NETACP_MAX_SESSIONS logical
 * links (each slot its own NSP link state -- a dnet_engine used ONLY for its
 * link FSM; routing/HELLO stay on the node's engine) and refuses a peer more
 * than NETACP_MAX_PER_SOURCE concurrent sessions, so one source cannot fill the
 * pool. The per-source cap is an OVMX hardening choice (LABELLED), not a VMS
 * parameter. Each session is still bounded in time by its own server: LOGINOUT's
 * login-read deadline for SET HOST, the FAL server's idle bound for FAL.
 *
 * THREE OBJECTS, ALL THE VMS WAY. Object 42 (CTERM): an executive-minted RTAn:
 * with LOGINOUT $CREPRC'd onto it; the remote authenticates fresh. Object 17
 * (FAL): the connect-carried credentials are authenticated at connect, then a
 * FAL.EXE network server process runs the access with the USER's UIC and
 * privileges (dnet_fal_proc.h). Object 27 (MAIL-11, rd vms-47fd): a
 * MAIL_SERVER.EXE network server process under the MAIL object's account
 * (dnet_mail_proc.h) receives the message and stores it in each recipient's
 * mail file. NETACP holds no credential beyond the check, serves no file and
 * stores no mail itself.
 */
/* rd vms-277a: a VMS DELETE node::file;* holds THREE links to the FAL at once
 * (two DIRECTORY LISTs still open, then the ERASE: VAX<->VAX capture
 * tests/lab/captures/decnet-fal-verbs-20261008/vax-to-vax-sys-login/); with a
 * share of 2 the third connect was refused reason 1 and the VAX printed
 * RMS-E-MKD / SYSTEM-F-REMRSRC (live bracket 2026-10-08). */
#define NETACP_MAX_PER_SOURCE 3
/* rd vms-f91: the pool's SIZE is the executor's MAXIMUM LINKS (NCP SET EXECUTOR
 * MAXIMUM LINKS; unset = the VMS default 32, dnet_ncpstore.h), read when NETACP
 * starts, as VMS bounds a node's logical links. NETACP_POOL_CAP is only this
 * image's slot-table capacity: a larger MAXIMUM LINKS is served up to it and
 * said so. The per-source share above stays an OVMX hardening choice. */
#define NETACP_POOL_CAP       64
static int g_netacp_max_links = DNET_EXECUTOR_DEFAULT_MAXLINKS;
#define NETACP_MAX_SESSIONS   g_netacp_max_links

/* The pool size an executor record gives (rd vms-f91): its MAXIMUM LINKS
 * (unset = the VMS default 32), served up to this image's slot table;
 * *clamped is set when the executor asked for more than the table holds. */
static int netacp_pool_size(const struct dnet_executor *x, int *clamped)
{
    unsigned want = dnet_executor_max_links(x);
    if (clamped) *clamped = (want > NETACP_POOL_CAP);
    return (int)(want > NETACP_POOL_CAP ? NETACP_POOL_CAP : want);
}

/* OUTBOUND links (rd vms-dda): a local process's $QIO IO$_ACCESS on _NET:
 * reaches NETACP as a broker OPEN and becomes a slot in THIS pool -- the same
 * bounded table, the same per-source discipline (here the source is the
 * requesting process), and an idle bound so a client that died cannot hold a
 * slot. Inbound data is queued per link until the client's READVBLK takes it;
 * a link whose queue overflows is aborted honestly (NSP here does not
 * retransmit data, so a dropped segment could never be recovered). */
#define NETACP_OUT_RXQ        32
#define NETACP_OUT_IDLE_SECS  60

struct netacp_slot {
    int      used;
    int      outbound;                  /* 1 = a local process's _NET: link   */
    int      object;                    /* 42 = CTERM, 17 = FAL, 27 = MAIL    */
    struct dnet_engine lk;              /* this session's NSP link state       */
    uint16_t peer;                      /* remote node (area<<10|node)         */
    uint8_t  peer_mac[6];
    struct dnet_cterm_host_session host;/* object 42: RTAn: + LOGINOUT        */
    struct dnet_cth cth;                /* object 42: the CTERM host wire role */
    int      term_recorded;             /* originating terminal on the RTAn:   */
    struct dnet_fal_proc fal;           /* objects 17 + 27: the server process */
    /* --- outbound (_NET:) links only --- */
    uint32_t handle;                    /* NETACP-issued link handle (unguessable) */
    uint32_t owner_pid;                 /* the requesting process              */
    uint32_t reply_unit;                /* its reply mailbox MBA<unit>:        */
    uint32_t reply_chan;                /* NETACP's channel to that mailbox    */
    uint32_t open_corr;                 /* an OPEN awaiting CC/DI (0 = none)   */
    int      remote_gone;               /* the remote disconnected / aborted   */
    uint16_t disc_reason;
    dnet_tick_t last_req;               /* last client request (idle bound)    */
    unsigned rxq_head, rxq_count;
    uint16_t rxq_len[NETACP_OUT_RXQ];
    uint8_t  rxq[NETACP_OUT_RXQ][DNET_NSP_MAX_DATA];
};

/* How long a session's terminal output must be quiet before the CTERM host
 * ships it -- and, with no read outstanding, solicits input with the trailing
 * prompt riding in the Start Read (rd vms-a70). */
#define NETACP_CTERM_IDLE_MS 60

/* The wire NETACP's sessions transmit on: the datalink, or (the booted pool
 * selftest only) a capture so the test plays the remote peers itself. */
static ssize_t (*g_netacp_tx)(int, int, uint16_t, const uint8_t *,
                              const uint8_t *, size_t) = scs_datalink_send;

/* LOCAL LOOPBACK (rd vms-dda). A logical link from this node to ITSELF --
 * $ COPY 0::file, or NODE:: naming this node, which VMS supports -- never
 * touches the wire: a frame NETACP addresses to its own DECnet id is queued
 * here and dispatched as if received, so the outbound slot and the inbound
 * session it reaches are two slots of the same pool, each running the real NSP
 * link FSM. Bounded; a full queue drops the frame (counted). */
#define NETACP_LOOP_MAX 64
static uint8_t  g_netacp_self[6];
static int      g_netacp_self_set;
static uint8_t  g_loopq[NETACP_LOOP_MAX][DNET_FRAME_MAX];
static size_t   g_loopq_len[NETACP_LOOP_MAX];
static unsigned g_loopq_head, g_loopq_count;
static unsigned long g_loopq_dropped;

static void netacp_send(int sock, unsigned ifindex, const uint8_t mac[6],
                        const uint8_t *f, size_t n)
{
    if (g_netacp_self_set && n <= DNET_FRAME_MAX && n >= 6 &&
        memcmp(f, g_netacp_self, 6) == 0) {
        if (g_loopq_count >= NETACP_LOOP_MAX) { g_loopq_dropped++; return; }
        unsigned t = (g_loopq_head + g_loopq_count) % NETACP_LOOP_MAX;
        memcpy(g_loopq[t], f, n);
        g_loopq_len[t] = n;
        g_loopq_count++;
        return;
    }
    (void)g_netacp_tx(sock, (int)ifindex, DNET_ETHERTYPE, mac, f, n);
}

/* Ship every CTERM segment the host FSM has queued, in order, over the link. */
static void netacp_cterm_drain(struct netacp_slot *sl, int sock, unsigned ifindex,
                               dnet_tick_t now)
{
    uint8_t seg[DNET_CTH_SEG_MAX], fr[DNET_FRAME_MAX];
    size_t n = 0, fn = 0;
    while (dnet_cth_tx_pop(&sl->cth, seg, sizeof seg, &n))
        if (dnet_engine_link_send(&sl->lk, seg, n, fr, sizeof fr, &fn, now) == 0)
            netacp_send(sock, ifindex, sl->peer_mac, fr, fn);
}

/* Typed input the remote server delivered goes to the session's terminal --
 * to LOGINOUT, which decides any login. */
static void netacp_cterm_feed_terminal(struct netacp_slot *sl)
{
    uint8_t in[256];
    size_t k;
    while ((k = dnet_cth_term_input(&sl->cth, in, sizeof in,
                                    dnet_cterm_host_echo(&sl->host))) > 0)
        (void)dnet_cterm_host_write(&sl->host, in, k);
}

static void netacp_outbound_release(struct netacp_slot *sl, const char *why);

/* End a session: tell the remote (CTERM unbind / NSP disconnect), release the
 * local side, free the slot. Safe on an unused slot. */
static void netacp_slot_end(struct netacp_slot *sl, int sock, unsigned ifindex,
                            dnet_tick_t now, const char *why)
{
    if (!sl->used) return;
    uint8_t buf[DNET_FRAME_MAX], fr[DNET_FRAME_MAX];
    size_t n = 0, fn = 0;
    (void)buf; (void)n;
    /* CTERM: the session's last output, then foundation Unbind (02 03 00,
     * "user unbind request") -- what a VMS host sends at logout. */
    if (sl->object == DNET_CTERM_OBJECT && sl->host.active && dnet_link_is_up(&sl->lk.link)) {
        uint8_t tail[1024];
        long got;
        while ((got = dnet_cterm_host_read(&sl->host, tail, sizeof tail)) > 0)
            (void)dnet_cth_term_output(&sl->cth, tail, (size_t)got, monotonic_ms());
        if (!dnet_cth_is_over(&sl->cth))
            (void)dnet_cth_close(&sl->cth);
        netacp_cterm_drain(sl, sock, ifindex, now);
    }
    if (dnet_link_is_up(&sl->lk.link) &&
        dnet_engine_link_close(&sl->lk, DNET_LINK_REASON_NORMAL, fr, sizeof fr, &fn, now) == 0)
        netacp_send(sock, ifindex, sl->peer_mac, fr, fn);
    if (sl->outbound) {
        netacp_outbound_release(sl, why);
        return;
    }
    if (sl->object == DNET_CTERM_OBJECT) (void)dnet_cterm_host_close(&sl->host);
    if (sl->object != DNET_CTERM_OBJECT) dnet_fal_proc_close(&sl->fal);
    log_ts(stdout);
    printf(" DECNETD-I-SESSEND, inbound %s session %s ended (%s)\n",
           sl->object == DNET_FAL_OBJECT ? "FAL" :
           sl->object == DNET_MAIL11_OBJECT ? "MAIL" : "SET HOST",
           sl->object == DNET_CTERM_OBJECT ? sl->host.devnam : "", why);
    fflush(stdout);
    memset(sl, 0, sizeof *sl);
}

static void netacp_outbound_tick(struct netacp_slot *sl, int sock, unsigned ifindex,
                                 dnet_tick_t now);

/* Pump every live session's local side once. Returns the number live. */
static int netacp_service_sessions(struct netacp_slot *slots, int sock,
                                   unsigned ifindex, dnet_tick_t now)
{
    int live = 0;
    for (int i = 0; i < NETACP_MAX_SESSIONS; i++) {
        struct netacp_slot *sl = &slots[i];
        if (!sl->used) continue;
        live++;
        uint8_t buf[DNET_FAL_SEG_MAX], fr[DNET_FRAME_MAX];
        size_t fn = 0;
        if (sl->outbound) {
            netacp_outbound_tick(sl, sock, ifindex, now);
            continue;
        }
        if (sl->object == DNET_CTERM_OBJECT) {
            /* The CTERM HOST speaks first (rd vms-a70): a real VMS SET HOST
             * client waits for the host's Bind Request. */
            uint64_t ms = monotonic_ms();
            if (sl->cth.state == DNET_CTH_S_IDLE)
                (void)dnet_cth_open(&sl->cth);
            /* Whatever LOGINOUT/DCL wrote to the session's RTAn: becomes CTERM
             * Writes, and its trailing prompt the Start Read that solicits the
             * next line -- the way a VMS host does it. */
            long got;
            int ended = 0;
            while ((got = dnet_cterm_host_read(&sl->host, buf, sizeof buf)) > 0)
                (void)dnet_cth_term_output(&sl->cth, buf, (size_t)got, ms);
            if (got < 0 || !dnet_cterm_host_alive(&sl->host))
                ended = 1;
            netacp_cterm_feed_terminal(sl);
            (void)dnet_cth_tick(&sl->cth, ms, dnet_cterm_host_echo(&sl->host));
            netacp_cterm_drain(sl, sock, ifindex, now);
            if (ended)
                /* Logged out, or LOGINOUT refused / timed out and exited --
                 * the executive's process table is the authority. */
                netacp_slot_end(sl, sock, ifindex, now, "the session process exited");
            (void)fr; (void)fn;
        } else {
            size_t n = 0; uint32_t xst = 0;
            int r;
            while ((r = dnet_fal_proc_poll(&sl->fal, buf, sizeof buf, &n, &xst)) == 1) {
                if (dnet_engine_link_send(&sl->lk, buf, n, fr, sizeof fr, &fn, now) == 0)
                    netacp_send(sock, ifindex, sl->peer_mac, fr, fn);
            }
            if (r == 2 || r < 0 || !dnet_fal_proc_alive(&sl->fal))
                netacp_slot_end(sl, sock, ifindex, now,
                                sl->object == DNET_MAIL11_OBJECT
                                    ? "the MAIL server finished" : "the FAL server finished");
        }
    }
    return live;
}

/* Refuse a Connect Initiate we hold no slot for, with `reason`. */
/* VMS names the remote port by NODE NAME when the local node database knows
 * the address ("VAX1::SYSTEM"; oracle tests/lab/captures/decnet-sethost-
 * inbound-20261005/vax-rta-show-terminal.txt), else by the numeric address
 * ("1025::SYSTEM"). Re-records the RTAn:'s remote port info with the name. */
static void netacp_rpi_by_name(struct dnet_cterm_host_session *hs, uint16_t peer)
{
    static struct dnet_nodedb db;
    const char *user = strstr(hs->remote_port_info, "::");
    if (!user || !hs->devnam[0] || dnet_store_load_nodes(&db) != DNET_STORE_OK)
        return;
    const struct dnet_node_entry *e = dnet_nodedb_by_addr(&db, peer);
    if (!e || !e->name[0])
        return;
    char named[sizeof hs->remote_port_info];
    int n = snprintf(named, sizeof named, "%s%s", e->name, user);
    if (n <= 0 || (size_t)n >= sizeof named)
        return;
    memcpy(hs->remote_port_info, named, (size_t)n + 1);
    (void)vms_kif_terminal_setrpi(hs->devnam, hs->remote_port_info);
}

/* The remote node as VMS MAIL names it in From: -- its node-database name,
 * else its decimal address (rd vms-47fd; the same rule as Remote Port Info). */
static void netacp_peer_name(uint16_t peer, char *out, size_t cap)
{
    static struct dnet_nodedb db;
    const struct dnet_node_entry *e = NULL;
    if (dnet_store_load_nodes(&db) == DNET_STORE_OK)
        e = dnet_nodedb_by_addr(&db, peer);
    if (e && e->name[0]) snprintf(out, cap, "%s", e->name);
    else                 snprintf(out, cap, "%u", (unsigned)peer);
}

static void netacp_refuse(struct dnet_engine *tmp, int sock, unsigned ifindex,
                          const uint8_t mac[6], uint16_t reason, dnet_tick_t now)
{
    uint8_t fr[DNET_FRAME_MAX]; size_t fn = 0;
    if (dnet_engine_link_close(tmp, reason, fr, sizeof fr, &fn, now) == 0)
        netacp_send(sock, ifindex, mac, fr, fn);
}

/*
 * ============ NETACP: OUTBOUND LINKS BROKERED OVER _NET: (rd vms-dda) ============
 *
 * On VMS every logical link -- inbound AND outbound -- belongs to NETACP; a user
 * process reaches one only through $ASSIGN _NET: + $QIO. The client side of that
 * (libvms qio_net_op) marshals each $QIO into a broker REQUEST on NETACP's
 * request mailbox (T1, docs/design-decnet-net-qio-mailbox-seam.md); this is the
 * NETACP side, run inside the same serve loop as the inbound session pool:
 *
 *   OPEN  (IO$_ACCESS)    parse the NCB, resolve the node from NETACP's node
 *                         database, build the Session Control connect with the
 *                         REQUESTING process's identity (read from the executive
 *                         by pid, never from the request), take a pool slot and
 *                         send the Connect Initiate. The reply is DEFERRED: it is
 *                         written when the remote answers -- Connect Confirm
 *                         (NORMAL + the link handle), a Disconnect (refused), or
 *                         the CI give-up (TIMEOUT).
 *   SEND  (IO$_WRITEVBLK) one NSP data segment on the link.
 *   RECV  (IO$_READVBLK)  the oldest queued inbound segment, ENDOFFILE if none
 *                         (the client re-asks; NETACP itself never blocks), ABORT
 *                         once the remote has gone and the queue is drained.
 *   CLOSE (IO$_DEACCESS)  Disconnect Initiate; the slot is freed.
 *
 * BOUNDS (vms-6af1 DoS discipline, the pool's own): outbound links share the
 * NETACP_MAX_SESSIONS slots with inbound sessions; one requesting process holds
 * at most NETACP_MAX_PER_SOURCE of them; a pool that is full or a requester over
 * its share is refused EXQUOTA with no link attempted; a link with no client
 * request for NETACP_OUT_IDLE_SECS (its process died) is disconnected and freed.
 * A request names its link by the handle NETACP issued at OPEN AND the owner pid
 * AND the reply unit; any mismatch is FILNOTACC, never another process's link.
 * Every request is bounds-decoded (dnet_broker_req_decode) before it is seen.
 */
static int g_netacp_serve_inbound = 1;   /* accept inbound CIs (object 42/17)   */

static void netacp_dispatch_frame(struct netacp_slot *slots, const struct dnet_engine *node,
                                  int sock, unsigned ifindex, const uint8_t *rx, size_t n,
                                  dnet_tick_t now, uint16_t *next_lla);

static int netacp_reply_mbx(struct netacp_slot *sl, uint32_t reply_unit,
                            const struct dnet_broker_rsp *rsp);
/* Where NETACP's broker responses go: the client's reply mailbox, or (the host
 * selftest) a capture standing in for the mailbox seam. */
static int (*g_netacp_reply)(struct netacp_slot *, uint32_t,
                             const struct dnet_broker_rsp *) = netacp_reply_mbx;

static int netacp_reply_mbx(struct netacp_slot *sl, uint32_t reply_unit,
                            const struct dnet_broker_rsp *rsp)
{
    uint8_t rec[DNET_BROKER_RSP_MAX];
    size_t n = 0;
    if (dnet_broker_rsp_encode(rsp, rec, sizeof rec, &n) != DNET_BROKER_OK)
        return -1;
    uint32_t ch = sl ? sl->reply_chan : 0;
    int transient = 0;
    if (!ch) {
        char dev[32];
        snprintf(dev, sizeof dev, "MBA%u:", (unsigned)reply_unit);
        if (!(vms_kif_mbx_assign(dev, &ch) & 1))
            return -1;                       /* the client's mailbox is gone */
        if (sl) sl->reply_chan = ch; else transient = 1;
    }
    /* IO$M_NORSWAIT (rd vms-c6d1): a client that never drains its reply
     * mailbox gets SS$_MBFULL for its reply -- NETACP's serve loop never
     * waits on a client. */
    uint32_t st = vms_kif_mbx_write_ex(ch, rec, (uint32_t)n, 1);
    if (st == SS$_MBFULL) {
        /* Dropped, not waited for: say so once per mailbox unit run. */
        static uint32_t last_full_unit;
        if (last_full_unit != reply_unit) {
            last_full_unit = reply_unit;
            log_ts(stdout);
            printf(" DECNETD-W-REPLYFULL, reply mailbox MBA%u: is full -- an answer"
                   " was dropped (the client is not reading it)\n", (unsigned)reply_unit);
            fflush(stdout);
        }
    }
    if (transient)
        (void)vms_kif_dassgn(ch);
    return (st & 1) ? 0 : -1;
}

static void netacp_respond(struct netacp_slot *sl, uint32_t reply_unit, uint32_t corr,
                           uint32_t status, const void *data, size_t len)
{
    struct dnet_broker_rsp rsp;
    memset(&rsp, 0, sizeof rsp);
    rsp.corr_id = corr;
    rsp.status  = status;
    if (data && len && len <= DNET_NSP_MAX_DATA) {
        memcpy(rsp.data, data, len);
        rsp.datalen = (uint16_t)len;
    }
    (void)g_netacp_reply(sl, reply_unit, &rsp);
}

/* An unguessable, nonzero link handle: a counter mixed with a per-run secret
 * from the kernel RNG (a guessed handle is still refused unless the owner pid
 * and reply unit also match). */
static uint32_t netacp_new_handle(void)
{
    static uint64_t key, ctr;
    static int seeded;
    if (!seeded) {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            if (read(fd, &key, sizeof key) != (ssize_t)sizeof key) key = 0;
            close(fd);
        }
        key ^= monotonic_ms() ^ ((uint64_t)getpid() << 32);
        seeded = 1;
    }
    for (;;) {
        uint64_t z = key + 0x9E3779B97F4A7C15ull * ++ctr;   /* splitmix64 */
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        if ((uint32_t)z != 0) return (uint32_t)z;
    }
}

/*
 * ================= NCP SHOW / SHOW NETWORK served from NETACP (rd vms-30e) =================
 * A _NET: IO$_ACPCONTROL reaches NETACP as DNET_BROKER_OP_SHOW (dnet_broker_control
 * in libvms's qio_net_op), and NETACP answers ONE dnet_netshow snapshot record
 * built HERE from its own live state -- nothing else answers it and nothing in it
 * is a constant dressed up as state (INV-6):
 *   executor   the NETACP engine's address/name/type, the identification this
 *              NETACP image carries, its circuit, the NSP version it connects
 *              with (NETACP_NSP_VERSION, the value its Connect Initiates carry),
 *              the routing version its hellos carry (DNET_ENGINE_ROUTING_*), its
 *              pool size, the links in use now, and the one executor counter it
 *              really keeps -- the high-water mark of links in use;
 *   nodes      the node database NETACP resolves NODE:: names against (read at
 *              request time, the same read broker_open_connect does) merged with
 *              its adjacency table, each with the links NETACP holds to it now;
 *   links      every used slot of the logical-link pool, inbound and outbound.
 * The query is bounds-validated (dnet_netshow_req_decode); a malformed one is
 * answered SS$_BADPARAM, never a partial record. Read-only: it changes nothing.
 */
#define NETACP_NSP_VERSION   DNET_NSP_VER_41    /* every Connect Initiate NETACP sends */
static unsigned g_netacp_links_hwm;            /* counter: Maximum logical links active */

static void netacp_note_links(const struct netacp_slot *slots)
{
    unsigned used = 0;
    for (int i = 0; i < NETACP_MAX_SESSIONS; i++)
        used += slots[i].used ? 1u : 0u;
    if (used > g_netacp_links_hwm)
        g_netacp_links_hwm = used;
}

static void netacp_show_name(const struct dnet_nodedb *db, uint16_t addr, char *out)
{
    const struct dnet_node_entry *e = db ? dnet_nodedb_by_addr(db, addr) : NULL;
    snprintf(out, DNET_NETSHOW_NAMEMAX + 1, "%s", e && e->name[0] ? e->name : "");
}

static void netacp_show_exec(const struct netacp_slot *slots, const struct dnet_engine *node,
                             struct dnet_netshow_exec *x)
{
    memset(x, 0, sizeof *x);
    x->addr = node->addr;
    x->state_on = 1;                                  /* this NETACP is serving */
    x->type = dnet_engine_is_router(node) ? DNET_NETSHOW_TYPE_ROUTING
                                          : DNET_NETSHOW_TYPE_NONROUTING;
    snprintf(x->name, sizeof x->name, "%s", node->node_name);
    /* The identification THIS NETACP image carries: OVMX's product id, never
     * the DEC/VSI mark (INV-0); <= 32 characters, the NCP bound. */
    int n = snprintf(x->ident, sizeof x->ident, "OVMX DECnet-compatible %s",
                     ovmx_product_version());
    if (n < 0 || (size_t)n >= sizeof x->ident)
        snprintf(x->ident, sizeof x->ident, "OVMX DECnet-compatible");
    snprintf(x->circuit, sizeof x->circuit, "%s", node->circuit);
    x->nsp_ver[0] = 4; x->nsp_ver[1] = 1; x->nsp_ver[2] = 0;   /* NETACP_NSP_VERSION */
    _Static_assert(NETACP_NSP_VERSION == DNET_NSP_VER_41,
                   "the NSP version NCP reports is the one NETACP connects with");
    x->rtg_ver[0] = DNET_ENGINE_ROUTING_VERSION;
    x->rtg_ver[1] = DNET_ENGINE_ROUTING_ECO;
    x->rtg_ver[2] = DNET_ENGINE_ROUTING_UECO;
    x->max_links = NETACP_MAX_SESSIONS;
    unsigned used = 0;
    for (int i = 0; i < NETACP_MAX_SESSIONS; i++)
        used += slots[i].used ? 1u : 0u;
    x->active_links = (uint16_t)used;
    x->max_links_active = (uint16_t)(g_netacp_links_hwm > 0xffff ? 0xffff : g_netacp_links_hwm);
    x->have_dr = node->have_dr ? 1 : 0;
    x->dr_addr = node->have_dr ? dnet_addr_from_id(node->dr_id) : 0;
}

static int netacp_show_addr_cmp(const void *a, const void *b)
{
    uint16_t x = ((const struct dnet_netshow_node *)a)->addr;
    uint16_t y = ((const struct dnet_netshow_node *)b)->addr;
    return (x > y) - (x < y);
}

/* Build NETACP's known-node table (DB + adjacencies, executor excluded),
 * sorted by address. Returns the count. */
static unsigned netacp_show_nodes(const struct netacp_slot *slots,
                                  const struct dnet_engine *node,
                                  const struct dnet_nodedb *db,
                                  struct dnet_netshow_node *out, unsigned cap)
{
    unsigned n = 0;
    for (unsigned i = 0; db && i < db->count && n < cap; i++) {
        const struct dnet_node_entry *e = dnet_nodedb_at(db, i);
        if (!e || e->addr == node->addr)
            continue;
        memset(&out[n], 0, sizeof out[n]);
        out[n].addr = e->addr;
        snprintf(out[n].name, sizeof out[n].name, "%s", e->name);
        out[n].flags = DNET_NETSHOW_NF_INDB;
        n++;
    }
    for (size_t k = 0; k < DNET_ADJ_MAX_NEIGHBORS; k++) {
        const struct dnet_adj_neighbor *a = &node->adj.nbr[k];
        if (!a->in_use || a->addr == node->addr)
            continue;
        unsigned j = 0;
        while (j < n && out[j].addr != a->addr)
            j++;
        if (j == n) {
            if (n >= cap)
                continue;
            memset(&out[n], 0, sizeof out[n]);
            out[n].addr = a->addr;
            netacp_show_name(db, a->addr, out[n].name);
            n++;
        }
        out[j].flags |= DNET_NETSHOW_NF_ADJ;
        out[j].adj_state = a->state == DNET_ADJ_UP ? DNET_NETSHOW_ADJ_UP
                         : a->state == DNET_ADJ_INITIALIZING ? DNET_NETSHOW_ADJ_INIT
                         : DNET_NETSHOW_ADJ_NONE;
    }
    uint16_t dr = node->have_dr ? dnet_addr_from_id(node->dr_id) : 0;
    for (unsigned j = 0; j < n; j++) {
        for (int i = 0; i < NETACP_MAX_SESSIONS; i++)
            if (slots[i].used && slots[i].peer == out[j].addr)
                out[j].active_links++;
        /* Next hop: a router reaches an adjacent node directly; an endnode sends
         * everything through its designated router, 0 while it has none (the
         * oracle endnode shows 0). */
        if (dnet_engine_is_router(node))
            out[j].next_node = out[j].adj_state == DNET_NETSHOW_ADJ_UP ? out[j].addr : 0;
        else
            out[j].next_node = dr;
    }
    qsort(out, n, sizeof out[0], netacp_show_addr_cmp);
    return n;
}

static void netacp_show(const struct netacp_slot *slots, const struct dnet_engine *node,
                        const struct dnet_broker_req *req)
{
    static struct dnet_netshow_rsp r;
    static struct dnet_netshow_node all[DNET_NODEDB_MAX + DNET_ADJ_MAX_NEIGHBORS];
    static struct dnet_nodedb db;
    struct dnet_netshow_req q;
    uint8_t out[DNET_NETSHOW_RSP_MAX + 16];
    size_t olen = 0;

    if (dnet_netshow_req_decode(req->data, req->datalen, &q) != DNET_NETSHOW_OK) {
        netacp_respond(NULL, req->reply_unit, req->corr_id, SS$_BADPARAM, NULL, 0);
        return;
    }
    memset(&r, 0, sizeof r);
    r.entity = q.entity;
    r.as_of = (uint32_t)time(NULL);
    netacp_show_exec(slots, node, &r.exec);

    int have_db = dnet_store_load_nodes(&db) == DNET_STORE_OK;
    if (q.entity == DNET_NETSHOW_ENT_NODES) {
        unsigned n = netacp_show_nodes(slots, node, have_db ? &db : NULL, all,
                                       (unsigned)(sizeof all / sizeof all[0]));
        r.total = (uint16_t)n;
        r.first = q.cursor > n ? (uint16_t)n : q.cursor;
        unsigned c = n - r.first;
        r.count = (uint8_t)(c > DNET_NETSHOW_NODES_PER ? DNET_NETSHOW_NODES_PER : c);
        memcpy(r.u.node, &all[r.first], r.count * sizeof all[0]);
    } else if (q.entity == DNET_NETSHOW_ENT_LINKS) {
        unsigned n = 0;
        for (int i = 0; i < NETACP_MAX_SESSIONS; i++)
            n += slots[i].used ? 1u : 0u;
        r.total = (uint16_t)n;
        r.first = q.cursor > n ? (uint16_t)n : q.cursor;
        unsigned k = 0;
        for (int i = 0; i < NETACP_MAX_SESSIONS && r.count < DNET_NETSHOW_LINKS_PER; i++) {
            const struct netacp_slot *sl = &slots[i];
            if (!sl->used)
                continue;
            if (k++ < r.first)
                continue;
            struct dnet_netshow_link *l = &r.u.link[r.count++];
            l->local_link = sl->lk.link.local_addr;
            l->remote_link = sl->lk.link.remote_addr;
            l->node = sl->peer;
            netacp_show_name(have_db ? &db : NULL, sl->peer, l->name);
            l->pid = sl->outbound ? sl->owner_pid
                   : sl->object == DNET_FAL_OBJECT ? sl->fal.pid
                   : sl->host.session_pid;
            l->object = (uint8_t)(sl->object < 0 || sl->object > 255 ? 0 : sl->object);
            l->outbound = sl->outbound ? 1 : 0;
            l->running = dnet_link_is_up(&sl->lk.link) ? 1 : 0;
        }
    }
    if (dnet_netshow_rsp_encode(&r, out, sizeof out, &olen) != DNET_NETSHOW_OK) {
        netacp_respond(NULL, req->reply_unit, req->corr_id, SS$_BADPARAM, NULL, 0);
        return;
    }
    netacp_respond(NULL, req->reply_unit, req->corr_id, SS$_NORMAL, out, olen);
}

/* Answer a pending OPEN (if any) and free the outbound slot. */
static void netacp_outbound_release(struct netacp_slot *sl, const char *why)
{
    if (sl->open_corr)
        netacp_respond(sl, sl->reply_unit, sl->open_corr, SS$_ABORT, NULL, 0);
    if (sl->reply_chan)
        (void)vms_kif_dassgn(sl->reply_chan);
    char pa[8];
    log_ts(stdout);
    printf(" DECNETD-I-LINKEND, outbound link to %s (object %d) for process %08X"
           " ended (%s)\n", dnet_addr_str(sl->peer, pa, sizeof pa), sl->object,
           (unsigned)sl->owner_pid, why);
    fflush(stdout);
    memset(sl, 0, sizeof *sl);
}

/* Timers for one outbound slot: CI retransmit / give-up while the OPEN waits,
 * and the idle bound once the link is the client's. */
static void netacp_outbound_tick(struct netacp_slot *sl, int sock, unsigned ifindex,
                                 dnet_tick_t now)
{
    if (sl->open_corr) {
        uint8_t fr[DNET_FRAME_MAX]; size_t fn = 0; int has = 0;
        if (dnet_engine_link_tick(&sl->lk, now, fr, sizeof fr, &fn, &has) == DNET_ENGINE_OK && has)
            netacp_send(sock, ifindex, sl->peer_mac, fr, fn);
        if (dnet_link_state_of(&sl->lk.link) == DNET_LINK_CLOSED) {
            /* No answer to the Connect Initiate within the give-up budget. */
            netacp_respond(sl, sl->reply_unit, sl->open_corr, SS$_TIMEOUT, NULL, 0);
            sl->open_corr = 0;
            sl->lk.link_active = 0;
            netacp_outbound_release(sl, "the remote node did not answer the connect");
        }
        return;
    }
    if (now - sl->last_req > NETACP_OUT_IDLE_SECS)
        netacp_slot_end(sl, sock, ifindex, now, "no request from its process -- idle bound");
}

/* A link event on an outbound slot (the frame was already given to its FSM). */
static void netacp_outbound_event(struct netacp_slot *sl, int sock, unsigned ifindex,
                                  enum dnet_link_event ev, dnet_tick_t now)
{
    uint8_t fr[DNET_FRAME_MAX]; size_t fn = 0;
    if (ev == DNET_LINK_EV_CONNECT_CONF && sl->open_corr) {
        /* NSP: the INITIATOR sends a LINK SERVICE right after the CC (it acks
         * the CC and opens the flow-control window) -- NETACP's job, not the
         * client's (rd vms-6165). */
        if (dnet_engine_link_service(&sl->lk, fr, sizeof fr, &fn, now) == DNET_ENGINE_OK)
            netacp_send(sock, ifindex, sl->peer_mac, fr, fn);
        uint8_t h[4] = { (uint8_t)sl->handle, (uint8_t)(sl->handle >> 8),
                         (uint8_t)(sl->handle >> 16), (uint8_t)(sl->handle >> 24) };
        netacp_respond(sl, sl->reply_unit, sl->open_corr, SS$_NORMAL, h, sizeof h);
        sl->open_corr = 0;
        sl->last_req = now;
        char pa[8];
        log_ts(stdout);
        printf(" DECNETD-I-LINKOUT, outbound link to %s object %d is RUN for process"
               " %08X\n", dnet_addr_str(sl->peer, pa, sizeof pa), sl->object,
               (unsigned)sl->owner_pid);
        fflush(stdout);
        return;
    }
    if (ev == DNET_LINK_EV_DISCONNECT || ev == DNET_LINK_EV_DISCONNECT_CONF) {
        sl->lk.link_active = 0;
        sl->disc_reason = sl->lk.link.disc_reason;
        if (sl->open_corr) {
            /* The remote refused the connect. Access control rejected is the
             * VMS INVLOGIN; anything else an honest ABORT carrying the reason. */
            uint8_t r[2] = { (uint8_t)sl->disc_reason, (uint8_t)(sl->disc_reason >> 8) };
            netacp_respond(sl, sl->reply_unit, sl->open_corr,
                           sl->disc_reason == DNET_LINK_REASON_ACCESS ? SS$_INVLOGIN
                                                                     : SS$_ABORT, r, 2);
            sl->open_corr = 0;
            netacp_outbound_release(sl, "the remote refused the connect");
            return;
        }
        sl->remote_gone = 1;               /* drained by RECV, freed by CLOSE */
        return;
    }
    if (ev == DNET_LINK_EV_DATA && !sl->remote_gone) {
        if (sl->rxq_count >= NETACP_OUT_RXQ) {
            /* The client is not draining its link: abort it rather than lose a
             * segment NSP here would never retransmit. */
            if (dnet_engine_link_close(&sl->lk, DNET_LINK_REASON_RESOURCE, fr, sizeof fr,
                                       &fn, now) == DNET_ENGINE_OK)
                netacp_send(sock, ifindex, sl->peer_mac, fr, fn);
            sl->lk.link_active = 0;
            sl->remote_gone = 1;
            sl->rxq_count = 0;
            return;
        }
        unsigned t = (sl->rxq_head + sl->rxq_count) % NETACP_OUT_RXQ;
        uint16_t n = sl->lk.rx_datalen > DNET_NSP_MAX_DATA ? DNET_NSP_MAX_DATA
                                                           : sl->lk.rx_datalen;
        memcpy(sl->rxq[t], sl->lk.rx_data, n);
        sl->rxq_len[t] = n;
        sl->rxq_count++;
    }
}

/*
 * netacp_broker_request - service ONE bounds-decoded broker request. Replies
 * through g_netacp_reply, at once or (an OPEN) when the remote answers.
 */
static void netacp_broker_request(struct netacp_slot *slots, const struct dnet_engine *node,
                                  int sock, unsigned ifindex,
                                  const struct dnet_broker_req *req, dnet_tick_t now,
                                  uint16_t *next_lla)
{
    uint16_t base = (uint16_t)(req->op & DNET_BROKER_OP_MASK);
    char pa[8];

    if (base == DNET_BROKER_OP_SHOW) {          /* NCP SHOW: read-only, no link */
        netacp_show(slots, node, req);
        return;
    }
    if (base == DNET_BROKER_OP_OPEN) {
        int free_i = -1, mine = 0;
        for (int i = 0; i < NETACP_MAX_SESSIONS; i++) {
            if (!slots[i].used) { if (free_i < 0) free_i = i; }
            else if (slots[i].outbound && slots[i].owner_pid == req->owner_pid) mine++;
        }
        if (free_i < 0 || mine >= NETACP_MAX_PER_SOURCE) {
            netacp_respond(NULL, req->reply_unit, req->corr_id, SS$_EXQUOTA, NULL, 0);
            log_ts(stdout);
            printf(" DECNETD-I-LINKREJ, outbound link for process %08X refused (%s)\n",
                   (unsigned)req->owner_pid,
                   free_i < 0 ? "all logical links are in use"
                              : "that process already holds its share of links");
            fflush(stdout);
            return;
        }
        unsigned ra = 0, rn = 0; int obj = 0;
        uint8_t desc[192]; size_t dlen = 0;
        uint32_t st = broker_open_connect(node, req->owner_pid, req->data, req->datalen,
                                          &ra, &rn, &obj, desc, sizeof desc, &dlen);
        if (!(st & 1)) {
            netacp_respond(NULL, req->reply_unit, req->corr_id, st, NULL, 0);
            return;
        }
        struct netacp_slot *sl = &slots[free_i];
        memset(sl, 0, sizeof *sl);
        sl->lk = *node;
        sl->lk.link_active = 0;
        sl->outbound   = 1;
        sl->object     = obj;
        sl->peer       = (uint16_t)(((ra & 0x3fu) << 10) | (rn & 0x3ffu));
        (void)dnet_id_from_addr(ra, rn, sl->peer_mac);
        sl->owner_pid  = req->owner_pid;
        sl->reply_unit = req->reply_unit;
        sl->handle     = netacp_new_handle();
        sl->last_req   = now;
        uint16_t lla = (*next_lla)++;
        if (*next_lla < 0x2100) *next_lla = 0x2100;
        uint8_t fr[DNET_FRAME_MAX]; size_t fn = 0;
        int orc = dnet_engine_link_open(&sl->lk, ra, rn, lla, desc, dlen, 1459, 1,
                                        NETACP_NSP_VERSION, fr, sizeof fr, &fn, now);
        memset(desc, 0, sizeof desc);      /* it carried the access-control password */
        if (orc != DNET_ENGINE_OK) {
            netacp_respond(NULL, req->reply_unit, req->corr_id, SS$_ABORT, NULL, 0);
            memset(sl, 0, sizeof *sl);
            return;
        }
        sl->used = 1;
        netacp_note_links(slots);
        sl->open_corr = req->corr_id;      /* answered on CC / DI / give-up */
        log_ts(stdout);
        printf(" DECNETD-I-LINKCI, process %08X: Connect Initiate to %s object %d\n",
               (unsigned)req->owner_pid, dnet_addr_str(sl->peer, pa, sizeof pa), obj);
        fflush(stdout);
        netacp_send(sock, ifindex, sl->peer_mac, fr, fn);
        return;
    }

    /* Every other op names an existing link: handle AND owner AND reply unit. */
    struct netacp_slot *sl = NULL;
    for (int i = 0; i < NETACP_MAX_SESSIONS && !sl; i++)
        if (slots[i].used && slots[i].outbound && req->link_handle != 0 &&
            slots[i].handle == req->link_handle && slots[i].owner_pid == req->owner_pid &&
            slots[i].reply_unit == req->reply_unit)
            sl = &slots[i];
    if (!sl || sl->open_corr) {
        netacp_respond(NULL, req->reply_unit, req->corr_id,
                       base == DNET_BROKER_OP_SEND || base == DNET_BROKER_OP_RECV ||
                       base == DNET_BROKER_OP_CLOSE ? SS$_FILNOTACC : SS$_ILLIOFUNC,
                       NULL, 0);
        return;
    }
    sl->last_req = now;
    uint8_t fr[DNET_FRAME_MAX]; size_t fn = 0;

    switch (base) {
    case DNET_BROKER_OP_SEND:
        if (sl->remote_gone || !dnet_link_is_up(&sl->lk.link)) {
            netacp_respond(sl, sl->reply_unit, req->corr_id, SS$_ABORT, NULL, 0);
            return;
        }
        if (dnet_engine_link_send(&sl->lk, req->data, req->datalen, fr, sizeof fr, &fn,
                                  now) != DNET_ENGINE_OK) {
            netacp_respond(sl, sl->reply_unit, req->corr_id, SS$_ABORT, NULL, 0);
            return;
        }
        netacp_send(sock, ifindex, sl->peer_mac, fr, fn);
        netacp_respond(sl, sl->reply_unit, req->corr_id, SS$_NORMAL, NULL, 0);
        return;

    case DNET_BROKER_OP_RECV:
        if (sl->rxq_count) {
            unsigned h = sl->rxq_head;
            netacp_respond(sl, sl->reply_unit, req->corr_id, SS$_NORMAL,
                           sl->rxq[h], sl->rxq_len[h]);
            sl->rxq_head = (h + 1) % NETACP_OUT_RXQ;
            sl->rxq_count--;
            return;
        }
        netacp_respond(sl, sl->reply_unit, req->corr_id,
                       sl->remote_gone ? SS$_ABORT : SS$_ENDOFFILE, NULL, 0);
        return;

    case DNET_BROKER_OP_CLOSE:
        if (!sl->remote_gone && dnet_link_is_up(&sl->lk.link) &&
            dnet_engine_link_close(&sl->lk, DNET_LINK_REASON_NORMAL, fr, sizeof fr, &fn,
                                   now) == DNET_ENGINE_OK)
            netacp_send(sock, ifindex, sl->peer_mac, fr, fn);
        sl->lk.link_active = 0;
        netacp_respond(sl, sl->reply_unit, req->corr_id, SS$_NORMAL, NULL, 0);
        netacp_outbound_release(sl, "its process disconnected");
        return;

    default:
        netacp_respond(sl, sl->reply_unit, req->corr_id, SS$_ILLIOFUNC, NULL, 0);
        return;
    }
}

/* Drain the loopback queue through the dispatch (frames NETACP sent itself). */
static void netacp_loop_drain(struct netacp_slot *slots, const struct dnet_engine *node,
                              int sock, unsigned ifindex, dnet_tick_t now,
                              uint16_t *next_lla)
{
    static uint8_t f[DNET_FRAME_MAX];
    for (int guard = 0; g_loopq_count && guard < 4 * NETACP_LOOP_MAX; guard++) {
        unsigned h = g_loopq_head;
        size_t n = g_loopq_len[h];
        memcpy(f, g_loopq[h], n);
        g_loopq_head = (h + 1) % NETACP_LOOP_MAX;
        g_loopq_count--;
        netacp_dispatch_frame(slots, node, sock, ifindex, f, n, now, next_lla);
    }
}

static void netacp_dispatch_frame(struct netacp_slot *slots, const struct dnet_engine *node,
                                  int sock, unsigned ifindex, const uint8_t *rx, size_t n,
                                  dnet_tick_t now, uint16_t *next_lla)
{
    uint8_t src_id[DNET_ADDR_LEN];
    const uint8_t *pdu = NULL; size_t pdu_len = 0;
    if (dnet_engine_parse_data_frame(rx, n, src_id, NULL, &pdu, &pdu_len) != DNET_ENGINE_OK)
        return;
    struct dnet_nsp_msg in;
    if (dnet_nsp_decode(pdu, pdu_len, &in, NULL) != DNET_NSP_OK)
        return;
    uint16_t peer = dnet_addr_from_id(src_id);
    const uint8_t *mac = rx + 6;
    {
        /* A logical-link frame is for THIS node only if routed to our id --
         * never act on a frame addressed elsewhere (or on our own transmission
         * echoed back by the datalink). */
        uint8_t dst_id[DNET_ADDR_LEN];
        const uint8_t *p2 = NULL; size_t l2 = 0;
        if (dnet_engine_parse_data_frame(rx, n, NULL, dst_id, &p2, &l2) != DNET_ENGINE_OK ||
            memcmp(dst_id, node->my_id, DNET_ADDR_LEN) != 0)
            return;
    }
    uint8_t reply[DNET_FRAME_MAX]; size_t rlen = 0; int has = 0;
    enum dnet_link_event ev = DNET_LINK_EV_NONE;

    /* Find this frame's session: by OUR link address, or -- a retransmitted
     * Connect Initiate -- by the peer's (node, link address). */
    struct netacp_slot *sl = NULL;
    for (int i = 0; i < NETACP_MAX_SESSIONS && !sl; i++) {
        struct netacp_slot *c = &slots[i];
        if (!c->used || c->peer != peer) continue;
        if (in.type == DNET_NSP_T_CI ? c->lk.link.remote_addr == in.srcaddr
                                     : c->lk.link.local_addr == in.dstaddr)
            sl = c;
    }

    if (sl) {
        if (dnet_engine_link_rx(&sl->lk, now, rx, n, reply, sizeof reply, &rlen,
                                &has, &ev) != 0)
            return;
        if (has) netacp_send(sock, ifindex, sl->peer_mac, reply, rlen);
        if (sl->outbound) {
            netacp_outbound_event(sl, sock, ifindex, ev, now);
            return;
        }
        if (ev == DNET_LINK_EV_DISCONNECT || ev == DNET_LINK_EV_DISCONNECT_CONF) {
            sl->lk.link_active = 0;
            netacp_slot_end(sl, sock, ifindex, now, "the remote disconnected");
            return;
        }
        if (ev != DNET_LINK_EV_DATA) return;
        if (sl->object != DNET_CTERM_OBJECT) {
            if (dnet_fal_proc_put(&sl->fal, sl->lk.rx_data, sl->lk.rx_datalen) != 0)
                netacp_slot_end(sl, sock, ifindex, now,
                                sl->object == DNET_MAIL11_OBJECT ? "MAIL server link lost"
                                                                 : "FAL server link lost");
            return;
        }
        /* CTERM: the host FSM decodes the server's segment (bounded --
         * these bytes arrive before anyone authenticated); typed lines go to
         * the session's terminal, to LOGINOUT, which decides any login. */
        int crc = dnet_cth_rx(&sl->cth, sl->lk.rx_data, sl->lk.rx_datalen, monotonic_ms());
        /* rd vms-14b: the remote's Initiate told us what terminal the SET HOST
         * came from (type, width, page). Record it on the RTAn: once, in the
         * executive, as VMS's RTAn: takes it from the originating terminal. */
        if (!sl->term_recorded && sl->cth.peer.term.valid) {
            uint32_t tst = dnet_cterm_host_record_origin(&sl->host, &sl->cth.peer.term);
            sl->term_recorded = 1;
            log_ts(stdout);
            printf(" DECNETD-I-RTATERM, %s takes the originating terminal: device type"
                   " %u, width %u, page %u (status %08X)\n", sl->host.devnam,
                   (unsigned)sl->cth.peer.term.devtype, (unsigned)sl->cth.peer.term.width,
                   (unsigned)sl->cth.peer.term.page, (unsigned)tst);
            fflush(stdout);
        }
        netacp_cterm_feed_terminal(sl);
        netacp_cterm_drain(sl, sock, ifindex, now);
        if (sl->cth.peer_unbound)
            netacp_slot_end(sl, sock, ifindex, now, "the remote unbound");
        else if (crc == DNET_CTH_EPROTO)
            netacp_slot_end(sl, sock, ifindex, now, "CTERM protocol error from the remote");
        return;
    }

    if (in.type != DNET_NSP_T_CI || !g_netacp_serve_inbound)
        return;                                 /* not ours: honestly dropped */

    /* A NEW Connect Initiate. Decode it on a scratch link first. */
    static struct dnet_engine tmp;
    tmp = *node;
    tmp.link_active = 0;
    if (dnet_engine_link_rx(&tmp, now, rx, n, reply, sizeof reply, &rlen, &has, &ev) != 0 ||
        ev != DNET_LINK_EV_CONNECT_IND)
        return;
    int obj = dnet_cterm_sc_connect_object(tmp.link.conn_data, tmp.link.conn_len);

    int free_i = -1, from_peer = 0;
    for (int i = 0; i < NETACP_MAX_SESSIONS; i++) {
        if (!slots[i].used) { if (free_i < 0) free_i = i; }
        else if (slots[i].peer == peer) from_peer++;
    }
    char pa[8];
    if (obj != DNET_CTERM_OBJECT && obj != DNET_FAL_OBJECT && obj != DNET_MAIL11_OBJECT) {
        netacp_refuse(&tmp, sock, ifindex, mac, DNET_LINK_REASON_OBJREJ, now);
        log_ts(stdout);
        printf(" DECNETD-I-CONNREJ, inbound connect from %s to object %d refused"
               " (no such object served here)\n", dnet_addr_str(peer, pa, sizeof pa), obj);
        fflush(stdout);
        return;
    }
    if (free_i < 0 || from_peer >= NETACP_MAX_PER_SOURCE) {
        netacp_refuse(&tmp, sock, ifindex, mac, DNET_LINK_REASON_RESOURCE, now);
        log_ts(stdout);
        printf(" DECNETD-I-CONNREJ, inbound connect from %s to object %d refused"
               " (%s)\n", dnet_addr_str(peer, pa, sizeof pa), obj,
               free_i < 0 ? "all inbound sessions are in use"
                          : "that node already holds its share of sessions");
        fflush(stdout);
        return;
    }

    uint8_t m11_acc[1 + DNET_M11_CONN_UDLEN];
    size_t  m11_acclen = 0;
    sl = &slots[free_i];
    memset(sl, 0, sizeof *sl);
    sl->lk = tmp;
    sl->peer = peer;
    sl->object = obj;
    memcpy(sl->peer_mac, mac, 6);
    sl->host.master_fd = -1;
    dnet_cth_init(&sl->cth, NETACP_CTERM_IDLE_MS);
    uint32_t cst;
    if (obj == DNET_CTERM_OBJECT) {
        /* THE ISOLATION SEAM (vms-515 §3.4): low-privilege bounded parse into a
         * validated descriptor; the privileged path sees only that. */
        struct dnet_conn_descriptor desc;
        if (dnet_conn_descriptor_from_wire(sl->lk.link.conn_data, sl->lk.link.conn_len,
                                           sl->lk.link.remote_node, &desc) != DNET_CTERM_OK)
            cst = SS$_BADPARAM;
        else
            cst = dnet_cterm_host_open_desc(&sl->host, &desc);
        if (!(cst & 1)) {
            netacp_refuse(&sl->lk, sock, ifindex, mac, DNET_LINK_REASON_OBJREJ, now);
            fprintf(stderr, "DECNETD-E-NOSESSION, inbound SET HOST refused: the session"
                    " could not be created (status %08X); no unauthenticated shell is"
                    " substituted\n", (unsigned)cst);
            memset(sl, 0, sizeof *sl);
            return;
        }
    } else if (obj == DNET_MAIL11_OBJECT) {
        /* MAIL-11 (rd vms-47fd): only a client whose connect user data this
         * server speaks is confirmed; the message is received and stored by a
         * MAIL_SERVER.EXE process under the MAIL object's account. No image, no
         * account, no process: refused -- nothing is ever acknowledged. */
        char rnode[16], lnode[16];
        netacp_peer_name(peer, rnode, sizeof rnode);
        snprintf(lnode, sizeof lnode, "%s", node->node_name);
        m11_acclen = 0;
        if (dnet_m11_connect_accept(sl->lk.link.conn_data, sl->lk.link.conn_len,
                                    m11_acc, sizeof m11_acc, &m11_acclen) != 0)
            cst = SS$_BADPARAM;
        else
            cst = dnet_mail_proc_start(&sl->fal, rnode, lnode);
        if (!(cst & 1)) {
            netacp_refuse(&sl->lk, sock, ifindex, mac,
                          cst == SS$_INVLOGIN ? DNET_LINK_REASON_ACCESS
                                              : DNET_LINK_REASON_OBJREJ, now);
            log_ts(stdout);
            printf(" DECNETD-I-CONNREJ, inbound MAIL connect from %s refused (status"
                   " %08X%s%s) -- no mail is accepted without a MAIL server\n",
                   dnet_addr_str(peer, pa, sizeof pa), (unsigned)cst,
                   sl->fal.fail_stage ? " at " : "",
                   sl->fal.fail_stage ? sl->fal.fail_stage : "");
            fflush(stdout);
            memset(sl, 0, sizeof *sl);
            return;
        }
    } else {
        /* FAL: authenticate the connect-carried credentials, then run the
         * access in a FAL.EXE server process AS THAT USER (rd vms-d85). */
        struct dnet_fal_identity id;
        cst = dnet_fal_connect_auth_id(sl->lk.link.conn_data, sl->lk.link.conn_len, &id);
        if (cst == SS$_NORMAL)
            cst = dnet_fal_proc_start(&sl->fal, id.uic, id.def_privs, id.username,
                                      id.default_dir);
        memset(&id, 0, sizeof id);
        if (!(cst & 1)) {
            netacp_refuse(&sl->lk, sock, ifindex, mac,
                          (cst == SS$_INVLOGIN || cst == SS$_NOPRIV)
                              ? DNET_LINK_REASON_ACCESS : DNET_LINK_REASON_OBJREJ, now);
            log_ts(stdout);
            printf(" DECNETD-I-CONNREJ, inbound FAL connect from %s refused (status"
                   " %08X) -- no file access without authentication\n",
                   dnet_addr_str(peer, pa, sizeof pa), (unsigned)cst);
            fflush(stdout);
            memset(sl, 0, sizeof *sl);
            return;
        }
    }
    sl->used = 1;
    netacp_note_links(slots);
    uint16_t lla = (*next_lla)++;
    if (*next_lla < 0x2100) *next_lla = 0x2100;
    uint8_t fr[DNET_FRAME_MAX]; size_t fn = 0;
    if ((obj == DNET_MAIL11_OBJECT
             ? dnet_engine_link_accept_data(&sl->lk, lla, m11_acc, m11_acclen, fr, sizeof fr,
                                            &fn, now)
             : dnet_engine_link_accept(&sl->lk, lla, fr, sizeof fr, &fn, now)) == 0)
        netacp_send(sock, ifindex, mac, fr, fn);
    if (obj == DNET_CTERM_OBJECT)
        netacp_rpi_by_name(&sl->host, peer);
    log_ts(stdout);
    if (obj == DNET_CTERM_OBJECT)
        printf(" DECNETD-I-SESSTART, inbound SET HOST accepted on %s -- LOGINOUT is"
               " authenticating (Remote Port Info: %s)\n", sl->host.devnam,
               sl->host.remote_port_info);
    else if (obj == DNET_MAIL11_OBJECT)
        printf(" DECNETD-I-MAILSTART, inbound MAIL from %s accepted -- received by"
               " MAIL_SERVER.EXE pid %08X as UIC [%o,%o]\n",
               dnet_addr_str(peer, pa, sizeof pa), (unsigned)sl->fal.pid,
               (unsigned)(sl->fal.uic >> 16), (unsigned)(sl->fal.uic & 0xffff));
    else
        printf(" DECNETD-I-FALSTART, inbound FAL access from %s accepted -- served by"
               " FAL.EXE pid %08X as UIC [%o,%o]\n", dnet_addr_str(peer, pa, sizeof pa),
               (unsigned)sl->fal.pid, (unsigned)(sl->fal.uic >> 16),
               (unsigned)(sl->fal.uic & 0xffff));
    fflush(stdout);
}


/*
 * --netacp-pool-selftest (rd vms-6af1, booted battery). Drives NETACP's REAL
 * dispatch (netacp_dispatch_frame / netacp_service_sessions) with this process
 * playing several remote peers: real Connect Initiates in, the replies captured
 * off NETACP's transmit hook. On the booted executive each accepted object-42
 * session is a REAL RTAn: + LOGINOUT ($CREPRC), so the proof is that:
 *   - a SECOND session is accepted while the first is live (no single slot);
 *   - a node holding its NETACP_MAX_PER_SOURCE sessions is refused another
 *     (reason 1, resources) while OTHER nodes are still admitted;
 *   - the pool caps at NETACP_MAX_SESSIONS, and a slot freed by a remote
 *     disconnect is reusable.
 * That is the R4 G2 property: one stalling peer can no longer deny SET HOST.
 */
static uint8_t g_cap[DNET_FRAME_MAX]; static size_t g_caplen; static int g_capn;
static ssize_t netacp_tx_capture(int fd, int ifx, uint16_t et, const uint8_t *mac,
                                 const uint8_t *f, size_t n)
{
    (void)fd; (void)ifx; (void)et; (void)mac;
    if (n <= sizeof g_cap) { memcpy(g_cap, f, n); g_caplen = n; g_capn++; }
    return (ssize_t)n;
}

/* Open a CTERM link from peer node `pn` with link address `lla`; returns 1 if
 * NETACP confirmed it, 0 if it refused (*reason set), -1 on a harness error. */
static int pool_connect_obj(struct dnet_engine *peer_eng, const struct dnet_engine *node,
                            struct netacp_slot *slots, uint16_t *next_lla, uint16_t lla,
                            uint16_t *reason, int obj, const char *user, const char *pw)
{
    uint8_t conn[128], f[DNET_FRAME_MAX], rep[DNET_FRAME_MAX];
    size_t cl = 0, fl = 0, rl = 0; int has = 0; enum dnet_link_event ev = DNET_LINK_EV_NONE;
    if (dnet_cterm_sc_connect_build(obj, "POOLT", 0x0001, 0x0004,
                                    user, pw, "", conn, sizeof conn, &cl) != 0 ||
        dnet_engine_link_open(peer_eng, dnet_area_of(node->addr), dnet_node_of(node->addr),
                              lla, conn, cl, 1459, 1, DNET_NSP_VER_41, f, sizeof f,
                              &fl, 10) != 0)
        return -1;
    g_capn = 0;
    netacp_dispatch_frame(slots, node, -1, 0, f, fl, 10, next_lla);
    if (g_capn == 0) return -1;
    if (dnet_engine_link_rx(peer_eng, 11, g_cap, g_caplen, rep, sizeof rep, &rl, &has, &ev) != 0)
        return -1;
    if (ev == DNET_LINK_EV_CONNECT_CONF) return 1;
    *reason = peer_eng->link.disc_reason;
    return 0;
}

static int pool_connect(struct dnet_engine *peer_eng, const struct dnet_engine *node,
                        struct netacp_slot *slots, uint16_t *next_lla, uint16_t lla,
                        uint16_t *reason)
{
    return pool_connect_obj(peer_eng, node, slots, next_lla, lla, reason,
                            DNET_CTERM_OBJECT, "", "");
}

static int run_netacp_pool_selftest(void)
{
    int pass = 0, fail = 0;
#define PL_CHECK(c, msg) do { if (c) { pass++; printf("  PASS: %s\n", msg); } \
    else { fail++; printf("  FAIL: %s\n", msg); } } while (0)
    /* The pool is sized from the executor record (rd vms-f91). Proven on the
     * sizing rule, then the run below uses a pool of 9, as after
     * NCP SET EXECUTOR MAXIMUM LINKS 9 (never touching this node's database). */
    {
        struct dnet_executor ex;
        memset(&ex, 0, sizeof ex);
        int cl = 0;
        PL_CHECK(netacp_pool_size(&ex, &cl) == 32 && !cl,
                 "an executor with no MAXIMUM LINKS set gets the VMS default pool of 32 (what a real VMS VAX node shows as Maximum links = 32)");
        ex.max_links = 9;
        PL_CHECK(netacp_pool_size(&ex, &cl) == 9 && !cl,
                 "executor MAXIMUM LINKS 9 sizes the inbound pool at 9");
        ex.max_links = 500;
        PL_CHECK(netacp_pool_size(&ex, &cl) == NETACP_POOL_CAP && cl,
                 "a MAXIMUM LINKS above the slot table is served up to the table and flagged, never overrun");
        ex.max_links = 9;
        g_netacp_max_links = netacp_pool_size(&ex, NULL);
    }
    printf("DECNETD-I-POOL, NETACP inbound session pool: %d sessions, %d per node"
           " (rd vms-6af1, R4 G2; size from executor MAXIMUM LINKS, rd vms-f91)\n",
           NETACP_MAX_SESSIONS, NETACP_MAX_PER_SOURCE);
    g_netacp_tx = netacp_tx_capture;
    static struct netacp_slot slots[NETACP_POOL_CAP];
    memset(slots, 0, sizeof slots);
    uint16_t next_lla = 0x2100;
    const uint8_t hw[6] = { 0x02,0,0,0,0,0x2a };
    static struct dnet_engine node, peers[NETACP_POOL_CAP + 2];
    dnet_engine_init(&node, 1, 42, "OVMX", "EWA0", NULL, hw, 0, 0, 0);

    /* Object 17 through the SAME dispatch: a bad password is refused at
     * connect with reason 34 (what a real VMS FAL answers, rd vms-a8a lab) and
     * no server is created; GUEST/GUEST is accepted onto a FAL.EXE server
     * process running as GUEST (rd vms-d85). */
    {
        static struct dnet_engine fe;
        uint8_t fhw[6] = { 0x02,0,0,0,4,0 };
        dnet_engine_init(&fe, 1, 300, "PEER", "EWA0", NULL, fhw, 0, 0, 0);
        uint16_t why = 0;
        int r = pool_connect_obj(&fe, &node, slots, &next_lla, 0x3e00, &why,
                                 DNET_FAL_OBJECT, "GUEST", "WRONGPW");
        PL_CHECK(r == 0 && why == DNET_LINK_REASON_ACCESS,
                 "an inbound FAL connect with a BAD password is refused at connect (reason 34, access control rejected)");
        dnet_engine_init(&fe, 1, 300, "PEER", "EWA0", NULL, fhw, 0, 0, 0);
        r = pool_connect_obj(&fe, &node, slots, &next_lla, 0x3e01, &why,
                             DNET_FAL_OBJECT, "GUEST", "GUEST");
        int ok = 0;
        for (int i = 0; i < NETACP_MAX_SESSIONS; i++)
            if (slots[i].used && slots[i].object == DNET_FAL_OBJECT &&
                slots[i].fal.uic == ((128u << 16) | 129u) && dnet_fal_proc_alive(&slots[i].fal))
                ok = 1;
        PL_CHECK(r == 1 && ok,
                 "an inbound FAL connect GUEST/GUEST is accepted onto a live FAL.EXE server process running as [128,129]");
        uint8_t f[DNET_FRAME_MAX]; size_t fl = 0;
        if (dnet_engine_link_close(&fe, DNET_LINK_REASON_NORMAL, f, sizeof f, &fl, 12) == 0)
            netacp_dispatch_frame(slots, &node, -1, 0, f, fl, 12, &next_lla);
        int used = 0;
        for (int i = 0; i < NETACP_MAX_SESSIONS; i++) used += slots[i].used;
        PL_CHECK(used == 0, "the FAL session ends when the remote disconnects (slot freed)");
    }

    /* Peer i is node 1.(100 + i/NETACP_MAX_PER_SOURCE): a full share each. */
    int confirmed = 0, refused_res = 0;
    for (int i = 0; i < NETACP_MAX_SESSIONS; i++) {
        uint8_t phw[6] = { 0x02,0,0,0,1,(uint8_t)i };
        dnet_engine_init(&peers[i], 1, (unsigned)(100 + i / NETACP_MAX_PER_SOURCE), "PEER", "EWA0", NULL, phw, 0, 0, 0);
        uint16_t why = 0;
        int r = pool_connect(&peers[i], &node, slots, &next_lla, (uint16_t)(0x3000 + i), &why);
        if (r == 1) confirmed++;
        else printf("  NOTE: session %d not confirmed (r=%d reason=%u)\n", i, r, why);
        if (i == 1)
            PL_CHECK(r == 1 && confirmed == 2,
                     "a SECOND inbound SET HOST is accepted while the first is live (no single slot)");
        if (i == 2)
            PL_CHECK(r == 1 && confirmed == 3,
                     "a THIRD session from the same node is accepted -- a VMS DELETE node::file;* holds three links at once (rd vms-277a)");
        if (i % NETACP_MAX_PER_SOURCE == NETACP_MAX_PER_SOURCE - 1) {
            /* that node now holds its share: a third from it is refused */
            static struct dnet_engine extra;
            uint8_t ehw[6] = { 0x02,0,0,0,2,(uint8_t)i };
            dnet_engine_init(&extra, 1, (unsigned)(100 + i / NETACP_MAX_PER_SOURCE), "PEER", "EWA0", NULL, ehw, 0, 0, 0);
            uint16_t why2 = 0;
            int r2 = pool_connect(&extra, &node, slots, &next_lla, (uint16_t)(0x3800 + i), &why2);
            if (r2 == 0 && why2 == DNET_LINK_REASON_RESOURCE) refused_res++;
        }
    }
    PL_CHECK(confirmed == NETACP_MAX_SESSIONS,
             "the pool admits NETACP_MAX_SESSIONS concurrent sessions, each a real RTAn: + LOGINOUT");
    PL_CHECK(refused_res == NETACP_MAX_SESSIONS / NETACP_MAX_PER_SOURCE,
             "a node already holding its share of sessions is REFUSED another (reason 1) while other nodes are admitted");
    {
        uint8_t phw[6] = { 0x02,0,0,0,3,0 };
        dnet_engine_init(&peers[NETACP_MAX_SESSIONS], 1, 200, "PEER", "EWA0", NULL, phw, 0, 0, 0);
        uint16_t why = 0;
        int r = pool_connect(&peers[NETACP_MAX_SESSIONS], &node, slots, &next_lla, 0x3f00, &why);
        PL_CHECK(r == 0 && why == DNET_LINK_REASON_RESOURCE,
                 "a fresh node is refused (reason 1) once the pool is full -- bounded, never unbounded");
    }
    /* A remote disconnect frees its slot, which is then reusable. */
    {
        uint8_t f[DNET_FRAME_MAX]; size_t fl = 0;
        if (dnet_engine_link_close(&peers[0], DNET_LINK_REASON_NORMAL, f, sizeof f, &fl, 20) == 0)
            netacp_dispatch_frame(slots, &node, -1, 0, f, fl, 20, &next_lla);
        int used = 0;
        for (int i = 0; i < NETACP_MAX_SESSIONS; i++) used += slots[i].used;
        uint16_t why = 0;
        uint8_t phw[6] = { 0x02,0,0,0,3,1 };
        dnet_engine_init(&peers[NETACP_MAX_SESSIONS + 1], 1, 201, "PEER", "EWA0", NULL, phw, 0, 0, 0);
        int r = pool_connect(&peers[NETACP_MAX_SESSIONS + 1], &node, slots, &next_lla, 0x3f01, &why);
        PL_CHECK(used == NETACP_MAX_SESSIONS - 1 && r == 1,
                 "a remote disconnect frees its slot and a new session reuses it");
    }
    for (int i = 0; i < NETACP_MAX_SESSIONS; i++)
        netacp_slot_end(&slots[i], -1, 0, 30, "selftest teardown");
    g_netacp_tx = scs_datalink_send;
    printf("DECNETD-I-POOL, %d passed, %d failed\n", pass, fail);
    if (fail == 0 && pass > 0) { printf("DECNETD-POOL-SELFTEST: PASS\n"); return 0; }
    printf("DECNETD-POOL-SELFTEST: FAIL\n");
    return 1;
#undef PL_CHECK
}

/*
 * ============== --mail11-accept-test (rd vms-47fd, booted battery) ==========
 * INBOUND DECnet MAIL-11 through NETACP's REAL dispatch (netacp_dispatch_frame
 * / netacp_service_sessions), this process playing the remote VAX1 (1.1) of the
 * oracle tests/lab/captures/decnet-mail11-20261008/ against a node named VAX2
 * (1.2): the oracle's own Connect Initiate and client segments go in, and every
 * reply NETACP's MAIL_SERVER.EXE process sends comes back off the transmit hook
 * and is compared BYTE FOR BYTE with what the real VAX2 sent. Session 1 is the
 * oracle's accepted message to SYSTEM -- stored in SYSTEM's mail file, where
 * the battery then reads it with OVMX MAIL; session 2 is NOSUCHUSER, refused
 * with the VAX's exact status and text. Needs /dev/vms, SYSUAF and
 * SYS$SYSTEM:MAIL_SERVER.EXE; with no image it says NOIMAGE and proves nothing.
 */
#define M11Q_MAX 16
static uint8_t g_m11q[M11Q_MAX][DNET_FRAME_MAX];
static size_t  g_m11q_len[M11Q_MAX];
static unsigned g_m11q_n;
static ssize_t m11_tx_capture(int fd, int ifx, uint16_t et, const uint8_t *mac,
                              const uint8_t *f, size_t n)
{
    (void)fd; (void)ifx; (void)et; (void)mac;
    if (g_m11q_n < M11Q_MAX && n <= DNET_FRAME_MAX) {
        memcpy(g_m11q[g_m11q_n], f, n);
        g_m11q_len[g_m11q_n++] = n;
    }
    return (ssize_t)n;
}

struct m11_rig {
    struct dnet_engine *peer;
    const struct dnet_engine *node;
    struct netacp_slot *slots;
    uint16_t *next_lla;
    dnet_tick_t tick;
};

/* Hand every captured NETACP frame to the peer; collect the DATA payloads it
 * receives into got[] and send the peer's acks back through dispatch. */
static void m11_drain(struct m11_rig *r, uint8_t got[][DNET_M11_MAX_REPLY], size_t *glen,
                      unsigned *ng, unsigned max)
{
    for (unsigned q = 0; q < g_m11q_n; q++) {
        uint8_t rep[DNET_FRAME_MAX]; size_t rl = 0; int has = 0;
        enum dnet_link_event ev = DNET_LINK_EV_NONE;
        if (dnet_engine_link_rx(r->peer, r->tick++, g_m11q[q], g_m11q_len[q], rep, sizeof rep,
                                &rl, &has, &ev) != 0)
            continue;
        if (ev == DNET_LINK_EV_DATA && *ng < max && r->peer->rx_datalen <= DNET_M11_MAX_REPLY) {
            memcpy(got[*ng], r->peer->rx_data, r->peer->rx_datalen);
            glen[(*ng)++] = r->peer->rx_datalen;
        }
        if (has) {
            unsigned keep = g_m11q_n; g_m11q_n = M11Q_MAX;     /* don't capture the echo */
            netacp_dispatch_frame(r->slots, r->node, -1, 0, rep, rl, r->tick++, r->next_lla);
            g_m11q_n = keep;
        }
    }
    g_m11q_n = 0;
}

/* Send one client segment; wait (<= 15 s) until `want` reply segments arrived. */
static unsigned m11_send(struct m11_rig *r, const void *seg, size_t n,
                         uint8_t got[][DNET_M11_MAX_REPLY], size_t *glen, unsigned want)
{
    uint8_t f[DNET_FRAME_MAX]; size_t fl = 0;
    unsigned ng = 0;
    g_m11q_n = 0;
    if (seg && dnet_engine_link_send(r->peer, seg, n, f, sizeof f, &fl, r->tick++) == 0)
        netacp_dispatch_frame(r->slots, r->node, -1, 0, f, fl, r->tick++, r->next_lla);
    m11_drain(r, got, glen, &ng, want ? want : 1);
    for (int i = 0; i < 1500 && ng < want; i++) {
        netacp_service_sessions(r->slots, -1, 0, r->tick++);
        m11_drain(r, got, glen, &ng, want);
        if (ng < want) { struct timespec ts = { 0, 10 * 1000 * 1000 }; nanosleep(&ts, NULL); }
    }
    return ng;
}

static int m11_live(const struct netacp_slot *slots)
{
    int n = 0;
    for (int i = 0; i < NETACP_MAX_SESSIONS; i++)
        n += slots[i].used && slots[i].object == DNET_MAIL11_OBJECT;
    return n;
}

static void m11_hex(const char *tag, const uint8_t *b, size_t n)
{
    printf("    %s (%zu):", tag, n);
    for (size_t i = 0; i < n && i < 64; i++) printf(" %02x", b[i]);
    printf("\n");
}

static int run_mail11_accept_test(void)
{
    printf("DECNETD-I-MAIL11, inbound DECnet MAIL-11 (object 27) through NETACP's real"
           " dispatch to a MAIL_SERVER.EXE process, replayed from the real VAX"
           " capture (rd vms-47fd)\n");
    if (!dnet_mail_proc_image_present()) {
        printf("DECNETD-I-MAIL11-NOIMAGE, SYS$SYSTEM:MAIL_SERVER.EXE is not on this system"
               " disk: no MAIL server can be created, so the MAIL-11 proof cannot run here\n");
        printf("DECNETD-MAIL11-ACCEPT: NOIMAGE\n");
        return 1;
    }
    int pass = 0, fail = 0;
#define M1_CHECK(c, msg) do { if (c) { pass++; printf("  PASS: %s\n", msg); } \
    else { fail++; printf("  FAIL: %s\n", msg); } } while (0)

    /* The From: the stored message carries is NODE::USER by the node database;
     * name 1.1 VAX1 for the proof and put the database back afterwards. */
    static struct dnet_nodedb saved, db;
    int have_saved = dnet_store_load_nodes(&saved) == DNET_STORE_OK;
    if (!have_saved) dnet_nodedb_init(&saved);
    db = saved;
    (void)dnet_nodedb_set(&db, (uint16_t)((1u << 10) | 1u), "VAX1");
    int named = dnet_store_save_nodes(&db) == DNET_STORE_OK;

    g_netacp_tx = m11_tx_capture;
    static struct netacp_slot slots[NETACP_POOL_CAP];
    memset(slots, 0, sizeof slots);
    uint16_t next_lla = 0x2100;
    static struct dnet_engine node, peer;
    const uint8_t hw2[6] = { 0xaa,0x00,0x04,0x00,0x02,0x04 };
    const uint8_t hw1[6] = { 0xaa,0x00,0x04,0x00,0x01,0x04 };
    dnet_engine_init(&node, 1, 2, "VAX2", "EWA0", NULL, hw2, 0, 0, 0);

    /* The oracle RCI's Session Control connect data (object 27, SYSTEM, USRDATA). */
    static const uint8_t conn[] = {
        0x00,0x1b, 0x02,0x00,0x1a,0x02,0x20,0x20,0x06,'S','Y','S','T','E','M',
        0x27, 0x00, 0x00, 0x00,
        0x10, 0x03,0x01,0x00,0x07,0x00,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x02,0x02,0x00,0x00 };
    static const uint8_t ok4[4] = { 0x01, 0x00, 0x00, 0x00 };
    static const uint8_t nsu4[4] = { 0x12, 0x81, 0x7e, 0x00 };
    static const char nsu_text[] = "%MAIL-E-NOSUCHUSR, no such user NOSUCHUSER at node VAX2";
    static const uint8_t z = 0;
    static uint8_t got[8][DNET_M11_MAX_REPLY];
    size_t glen[8];
    struct m11_rig rig = { &peer, &node, slots, &next_lla, 100 };

    for (int sess = 0; sess < 2; sess++) {
        dnet_engine_init(&peer, 1, 1, "VAX1", "EWA0", NULL, hw1, 0, 0, 0);
        uint8_t f[DNET_FRAME_MAX]; size_t fl = 0;
        g_m11q_n = 0;
        if (dnet_engine_link_open(&peer, 1, 2, (uint16_t)(0x2015 + sess), conn, sizeof conn,
                                  1459, 1, DNET_NSP_VER_41, f, sizeof f, &fl, rig.tick++) != 0) {
            M1_CHECK(0, "the oracle Connect Initiate could be built");
            break;
        }
        netacp_dispatch_frame(slots, &node, -1, 0, f, fl, rig.tick++, &next_lla);
        int confirmed = 0, ccdata_ok = 0;
        for (unsigned q = 0; q < g_m11q_n; q++) {
            uint8_t sid[DNET_ADDR_LEN]; const uint8_t *pdu = NULL; size_t pl = 0;
            struct dnet_nsp_msg cc;
            if (dnet_engine_parse_data_frame(g_m11q[q], g_m11q_len[q], sid, NULL, &pdu, &pl) ==
                    DNET_ENGINE_OK && dnet_nsp_decode(pdu, pl, &cc, NULL) == DNET_NSP_OK &&
                cc.type == DNET_NSP_T_CC)
                ccdata_ok = cc.datalen == 17 && cc.data[0] == 0x10 &&
                            memcmp(cc.data + 1, dnet_m11_accept_userdata, 16) == 0;
            uint8_t rep[DNET_FRAME_MAX]; size_t rl = 0; int has = 0;
            enum dnet_link_event ev = DNET_LINK_EV_NONE;
            if (dnet_engine_link_rx(&peer, rig.tick++, g_m11q[q], g_m11q_len[q], rep, sizeof rep,
                                    &rl, &has, &ev) == 0 && ev == DNET_LINK_EV_CONNECT_CONF)
                confirmed = 1;
        }
        g_m11q_n = 0;
        if (sess == 0) {
            M1_CHECK(confirmed && m11_live(slots) == 1,
                     "the oracle's object-27 connect is CONFIRMED and lands on a MAIL_SERVER.EXE process");
            M1_CHECK(ccdata_ok,
                     "the Connect Confirm carries the VAX MAIL_SERVER's 16-byte accept data, byte-exact");
            for (int i = 0; i < NETACP_MAX_SESSIONS; i++)
                if (slots[i].used && slots[i].object == DNET_MAIL11_OBJECT)
                    printf("    MAIL_SERVER.EXE pid %08X runs as UIC [%o,%o]\n",
                           (unsigned)slots[i].fal.pid, (unsigned)(slots[i].fal.uic >> 16),
                           (unsigned)(slots[i].fal.uic & 0xffff));
        }
        if (!confirmed) { M1_CHECK(0, "session confirmed"); break; }

        unsigned ng = m11_send(&rig, "SYSTEM      ", 12, got, glen, 0);
        if (sess == 0) {
            ng = m11_send(&rig, "SYSTEM", 6, got, glen, 1);
            M1_CHECK(ng == 1 && glen[0] == 4 && memcmp(got[0], ok4, 4) == 0,
                     "recipient SYSTEM is ACCEPTED: 01 00 00 00, as the VAX answered");
            static const char *const recs[] = {
                "", "VAX2::SYSTEM", "", "DECnet MAIL-11 oracle",
                "Line one of a MAIL-11 message from VAX1.", "Line two." };
            (void)m11_send(&rig, &z, 1, got, glen, 0);
            for (unsigned i = 1; i < sizeof recs / sizeof recs[0]; i++)
                (void)m11_send(&rig, recs[i], strlen(recs[i]), got, glen, 0);
            ng = m11_send(&rig, &z, 1, got, glen, 1);
            if (ng) m11_hex("final status", got[0], glen[0]);
            M1_CHECK(ng == 1 && glen[0] == 4 && memcmp(got[0], ok4, 4) == 0,
                     "the message is STORED in SYSTEM's mail file and acknowledged 01 00 00 00"
                     " -- the VAX's final status, sent only after the store succeeded");
        } else {
            ng = m11_send(&rig, "NOSUCHUSER", 10, got, glen, 3);
            if (ng) m11_hex("refusal status", got[0], glen[0]);
            M1_CHECK(ng == 3 && glen[0] == 4 && memcmp(got[0], nsu4, 4) == 0 &&
                     glen[1] == strlen(nsu_text) && memcmp(got[1], nsu_text, glen[1]) == 0 &&
                     glen[2] == 1 && got[2][0] == 0,
                     "recipient NOSUCHUSER is REFUSED with the VAX's exact bytes: 12 81 7E 00,"
                     " '%MAIL-E-NOSUCHUSR, no such user NOSUCHUSER at node VAX2', 00");
        }
        /* The client disconnects, as the VAX did; the server process finishes. */
        g_m11q_n = 0;
        if (dnet_engine_link_close(&peer, DNET_LINK_REASON_NORMAL, f, sizeof f, &fl, rig.tick++) == 0)
            netacp_dispatch_frame(slots, &node, -1, 0, f, fl, rig.tick++, &next_lla);
        g_m11q_n = 0;
        for (int i = 0; i < 300 && m11_live(slots); i++) {
            netacp_service_sessions(slots, -1, 0, rig.tick++);
            struct timespec ts = { 0, 10 * 1000 * 1000 }; nanosleep(&ts, NULL);
        }
        if (sess == 1)
            M1_CHECK(m11_live(slots) == 0, "each session ends when the remote disconnects (slot freed)");
    }

    /* rd vms-47fd: the mail file belongs to its USER even though MAIL_SERVER
     * (another account) may have created it -- VMS's MAIL_SERVER creates a
     * recipient's mail file owned by the recipient. Read the owner from the
     * file header. */
    {
        sysuaf_record_t sr;
        struct rms_fileattr fa;
        char spec[600] = "";
        int owner_ok = 0;
        if (sysuaf_lookup("SYSTEM", &sr) == 0) {
            /* the spec MAIL_SERVER's mail_store_spec builds: <defdir>OVMX_MAIL.MAI */
            snprintf(spec, sizeof spec, "%sOVMX_MAIL.MAI", sr.default_dir);
            owner_ok = (rms_file_attr(spec, &fa) & 1) &&
                       fa.uic_group == sr.uic_group && fa.uic_member == sr.uic_member;
        }
        memset(&sr, 0, sizeof sr);
        M1_CHECK(owner_ok, "SYSTEM's mail file is owned by SYSTEM (the recipient), not by the"
                           " MAIL_SERVER account that delivered into it");
    }

    /* A connect this server does not speak (no MAIL-11 user data) is REFUSED --
     * nothing is confirmed, no process is created. */
    {
        static const uint8_t bare[] = { 0x00,0x1b, 0x02,0x00,0x1a,0x02,0x20,0x20,0x06,
                                        'S','Y','S','T','E','M', 0x00 };
        dnet_engine_init(&peer, 1, 1, "VAX1", "EWA0", NULL, hw1, 0, 0, 0);
        uint8_t f[DNET_FRAME_MAX], rep[DNET_FRAME_MAX]; size_t fl = 0, rl = 0; int has = 0;
        enum dnet_link_event ev = DNET_LINK_EV_NONE;
        g_m11q_n = 0;
        int refused = 0;
        if (dnet_engine_link_open(&peer, 1, 2, 0x2030, bare, sizeof bare, 1459, 1,
                                  DNET_NSP_VER_41, f, sizeof f, &fl, rig.tick++) == 0) {
            netacp_dispatch_frame(slots, &node, -1, 0, f, fl, rig.tick++, &next_lla);
            for (unsigned q = 0; q < g_m11q_n; q++)
                if (dnet_engine_link_rx(&peer, rig.tick++, g_m11q[q], g_m11q_len[q], rep,
                                        sizeof rep, &rl, &has, &ev) == 0 &&
                    (ev == DNET_LINK_EV_DISCONNECT || ev == DNET_LINK_EV_DISCONNECT_CONF))
                    refused = 1;
        }
        g_m11q_n = 0;
        M1_CHECK(refused && m11_live(slots) == 0,
                 "an object-27 connect without MAIL-11 user data is REFUSED (no confirm, no server)");
    }

    for (int i = 0; i < NETACP_MAX_SESSIONS; i++)
        netacp_slot_end(&slots[i], -1, 0, rig.tick++, "selftest teardown");
    g_netacp_tx = scs_datalink_send;
    if (named && have_saved) {
        (void)dnet_store_save_nodes(&saved);           /* put the original back */
    } else if (named) {
        /* There was NO node database before this test: remove every version
         * it wrote, so the system is left exactly as it was found. */
        const char *ns = dnet_store_vms_spec(DNET_STORE_NODES);
        int erased = 0;
        for (int k = 0; k < 32; k++) {
            struct FAB ef = cc$rms_fab;
            ef.fab$l_fna = (char *)ns;
            ef.fab$b_fns = (uint8_t)strlen(ns);
            if (!(sys$erase(&ef, 0, 0) & 1)) break;
            erased++;
        }
        struct rms_fileattr fa;
        int gone = !(rms_file_attr(ns, &fa) & 1);
        printf("  %s: the node database this test created is removed again (%d version(s) erased)\n",
               gone ? "PASS" : "FAIL", erased);
        if (gone) pass++; else fail++;
    } else {
        printf("  NOTE: the node database could not name 1.1 VAX1 (From: shows the address)\n");
    }
    printf("DECNETD-I-MAIL11, %d passed, %d failed\n", pass, fail);
    if (fail == 0 && pass > 0) { printf("DECNETD-MAIL11-ACCEPT: PASS\n"); return 0; }
    printf("DECNETD-MAIL11-ACCEPT: FAIL\n");
    return 1;
#undef M1_CHECK
}

/*
 * NETACP's broker request mailbox (rd vms-dda, T1): created at startup and
 * published as the LNM$SYSTEM logical DNET$NETACP_REQ, which is how a _NET:
 * $QIO finds the running NETACP (and how a client knows none is running). With
 * no executive mailbox the broker is honestly NOT offered -- the inbound pool
 * still serves, and outbound clients get SS$_DEVOFFLINE.
 */
/*
 * The request mailbox's protection (rd vms-c6d1): S:RWLP,O:RWLP,G:,W:W. Every
 * process may WRITE a request into it; only NETACP itself (its owner, a SYSTEM-
 * category UIC) -- or a holder of SYSPRV or BYPASS, exactly as real VMS rules a
 * mailbox (READALL does not open one: oracle mbxprot) -- may READ it, so no client can dequeue another client's request and
 * the NCB access-control password in it. Enforced by the executive's one
 * protection decision (src/kernel-core/vms_prot.h), the same one a file gets.
 */
#define NETACP_REQ_PROMSK 0xDF00u

static int netacp_broker_start(uint32_t *chan)
{
    uint32_t unit = 0;
    char dev[64] = "";
    uint32_t st = vms_kif_mbx_create_prot(0, DNET_BROKER_REQ_MAX + 16,
                                          (DNET_BROKER_REQ_MAX + 16) * 32,
                                          NETACP_REQ_PROMSK, chan, &unit, dev,
                                          sizeof dev);
    if (!(st & 1)) {
        log_ts(stdout);
        printf(" DECNETD-W-NOBROKER, could not create the _NET: request mailbox"
               " (status %08X) -- outbound links are not offered\n", (unsigned)st);
        fflush(stdout);
        *chan = 0;
        return 0;
    }
    const char *vals[1] = { dev };
    st = vms_kif_lnm_define(VMS_LNM_TBL_SYSTEM, NETACP_REQ_LOGNAM, vals, 1, 0,
                            LNM$C_USER);
    if (!(st & 1)) {
        (void)vms_kif_mbx_delmbx(*chan);
        (void)vms_kif_dassgn(*chan);
        log_ts(stdout);
        printf(" DECNETD-W-NOBROKER, could not publish %s (status %08X) -- outbound"
               " links are not offered\n", NETACP_REQ_LOGNAM, (unsigned)st);
        fflush(stdout);
        *chan = 0;
        return 0;
    }
    log_ts(stdout);
    printf(" DECNETD-I-BROKER, outbound logical links are brokered for local"
           " processes ($ASSIGN _NET:): request mailbox %s = %s; up to %d links"
           " in the pool, %d per process\n", dev, NETACP_REQ_LOGNAM,
           NETACP_MAX_SESSIONS, NETACP_MAX_PER_SOURCE);
    fflush(stdout);
    return 1;
}

static void netacp_broker_stop(uint32_t chan)
{
    (void)vms_kif_lnm_delete(VMS_LNM_TBL_SYSTEM, NETACP_REQ_LOGNAM, LNM$C_USER);
    (void)vms_kif_mbx_delmbx(chan);
    (void)vms_kif_dassgn(chan);
}

/* Take every request waiting in the mailbox (bounded per pass, IO$M_NOW --
 * NETACP never blocks here) and service it. A record that fails the
 * bounds-validated decode is dropped: its reply unit is untrusted. */
static unsigned long g_broker_malformed;
/* Requests dropped because the executive says their writer is not the process
 * they claim (owner_pid), or the writer does not hold NETMBX (rd vms-c6d1). */
static unsigned long g_broker_forged, g_broker_nonetmbx;
/*
 * Decode + service one raw broker request record (the mailbox drain and the
 * host selftest's in-process queue both come through here).
 *
 * WHO SENT IT (rd vms-c6d1). `verify` is set for a record read from the
 * executive mailbox, and `sender_pid` is then the executive's own stamp of the
 * process that wrote it (the mailbox read's IOSB second longword) -- not
 * something the writer can assert. A request whose owner_pid is not its writer
 * is a forgery and is dropped unanswered (its reply unit is the forger's
 * choice); so is one whose writer does not hold NETMBX, the privilege VMS
 * requires of every network user (the same one $ASSIGN _NET: demands) -- read
 * from the executive ($GETJPI of the stamped PID), never from the request. The
 * host selftest's in-process queue has no executive and passes verify = 0.
 */
static void netacp_broker_record(struct netacp_slot *slots, const struct dnet_engine *node,
                                 int sock, unsigned ifindex, const uint8_t *rec, size_t len,
                                 int verify, uint32_t sender_pid,
                                 dnet_tick_t now, uint16_t *next_lla)
{
    static struct dnet_broker_req req;
    if (dnet_broker_req_decode(rec, len, &req) != DNET_BROKER_OK) {
        g_broker_malformed++;
        return;                            /* untrusted reply unit: no answer */
    }
    if (verify) {
        struct vms_procinfo pi;
        if (sender_pid == 0 || req.owner_pid != sender_pid) {
            g_broker_forged++;
            return;                        /* not who it says: no answer */
        }
        memset(&pi, 0, sizeof pi);
        if (!(vms_kif_getjpi_pid(sender_pid, &pi) & 1) || pi.redacted ||
            !(pi.cur_privs & PRV$M_NETMBX)) {
            g_broker_nonetmbx++;
            return;                        /* no NETMBX: not a network user */
        }
    }
    netacp_broker_request(slots, node, sock, ifindex, &req, now, next_lla);
}

static void netacp_broker_drain(uint32_t chan, struct netacp_slot *slots,
                                const struct dnet_engine *node, int sock,
                                unsigned ifindex, dnet_tick_t now, uint16_t *next_lla)
{
    static uint8_t buf[DNET_BROKER_REQ_MAX + 16];
    for (int k = 0; k < 32; k++) {
        uint32_t got = 0, sender = 0;
        uint32_t st = vms_kif_mbx_read_ex(chan, buf, sizeof buf, &got, 1, &sender);
        if (!(st & 1))
            return;                       /* SS$_ENDOFFILE: nothing waiting */
        netacp_broker_record(slots, node, sock, ifindex, buf,
                             got > sizeof buf ? sizeof buf : got, 1, sender,
                             now, next_lla);
    }
}

/*
 * ===================== --netacp-broker-selftest (rd vms-dda) =====================
 * The HOST FLOOR of outbound links brokered through NETACP: a real NETACP pool
 * (netacp_broker_request / netacp_dispatch_frame / netacp_service_sessions, the
 * code the daemon runs) on node 1.10, a second engine on node 1.11 across a
 * socketpair datalink, and clients that reach NETACP ONLY through broker
 * records -- dnet_broker_xfer, the same marshalling libvms's qio_net_op runs,
 * over an in-process queue standing in for the executive mailboxes. No
 * executive, no CAP_NET_RAW. It proves:
 *   - $ COPY's FAL client (copy_client_run_net, what DECNETD --copy runs on a
 *     booted node) opens an object-17 link through NETACP -- the OPEN completes
 *     only when the remote's Connect Confirm arrives -- drives a DAP session
 *     both ways over SEND/RECV, and the honest miss round-trips; DEACCESS
 *     frees the slot;
 *   - a remote access-control rejection completes the OPEN as SS$_INVLOGIN;
 *   - a link from this node to ITSELF (0::) goes over NETACP's local loopback
 *     and reaches its own inbound dispatch (no such object -> refused);
 *   - bounds: a process over its share and a full pool are refused EXQUOTA
 *     with no Connect Initiate sent; pending opens are answered on teardown;
 *   - correlation: a stale / mis-routed / malformed response is dropped by the
 *     client and only the matching one is delivered; a request naming another
 *     process's link is refused FILNOTACC; a silent NETACP is DEVOFFLINE.
 */
struct nb_rq { uint8_t rec[DNET_BROKER_REQ_MAX]; size_t len; };
#define NB_QMAX 64
static struct nb_rq g_nbreq[NB_QMAX];      /* the request "mailbox"           */
static unsigned g_nbreq_n;
struct nb_reply { uint32_t unit; uint8_t rec[DNET_BROKER_RSP_MAX]; size_t len; };
static struct nb_reply g_nbrep[NB_QMAX];   /* responses, routed by reply unit */
static unsigned g_nbrep_n;
static unsigned long g_nb_wire;            /* frames NETACP put on the wire   */
static int g_nb_wire_fd = -1;              /* -1: discard (the bounds tests)  */
static struct netacp_slot g_nbslots[NETACP_POOL_CAP];
static struct dnet_engine g_nbnode;
static uint16_t g_nb_lla = 0x2100;

static ssize_t nb_tx(int fd, int ifx, uint16_t et, const uint8_t *mac,
                     const uint8_t *f, size_t n)
{
    (void)fd; (void)ifx; (void)et; (void)mac;
    g_nb_wire++;
    if (g_nb_wire_fd >= 0)
        return write(g_nb_wire_fd, f, n);
    return (ssize_t)n;
}
static int nb_reply_capture(struct netacp_slot *sl, uint32_t unit,
                            const struct dnet_broker_rsp *rsp)
{
    (void)sl;
    if (g_nbrep_n >= NB_QMAX) return -1;
    struct nb_reply *r = &g_nbrep[g_nbrep_n];
    if (dnet_broker_rsp_encode(rsp, r->rec, sizeof r->rec, &r->len) != DNET_BROKER_OK)
        return -1;
    r->unit = unit;
    g_nbrep_n++;
    return 0;
}
/* Take the oldest captured response for `unit` (0 = none). */
static int nb_take_reply(uint32_t unit, uint8_t *buf, size_t cap, size_t *len)
{
    for (unsigned i = 0; i < g_nbrep_n; i++) {
        if (g_nbrep[i].unit != unit) continue;
        if (g_nbrep[i].len > cap) return -1;
        memcpy(buf, g_nbrep[i].rec, g_nbrep[i].len);
        *len = g_nbrep[i].len;
        memmove(&g_nbrep[i], &g_nbrep[i + 1], (g_nbrep_n - i - 1) * sizeof g_nbrep[0]);
        g_nbrep_n--;
        return 1;
    }
    return 0;
}

/* One NETACP serve-loop pass: requests, wire frames, loopback, timers. */
static void nb_pump(void)
{
    dnet_tick_t now = monotonic_sec();
    for (unsigned i = 0; i < g_nbreq_n; i++)
        netacp_broker_record(g_nbslots, &g_nbnode, g_nb_wire_fd, 0, g_nbreq[i].rec,
                             g_nbreq[i].len, 0, 0, now, &g_nb_lla);
    g_nbreq_n = 0;
    if (g_nb_wire_fd >= 0) {
        uint8_t f[DNET_FRAME_MAX];
        ssize_t n;
        while ((n = recv(g_nb_wire_fd, f, sizeof f, MSG_DONTWAIT)) > 0)
            netacp_dispatch_frame(g_nbslots, &g_nbnode, g_nb_wire_fd, 0, f, (size_t)n,
                                  now, &g_nb_lla);
    }
    netacp_loop_drain(g_nbslots, &g_nbnode, g_nb_wire_fd, 0, now, &g_nb_lla);
    (void)netacp_service_sessions(g_nbslots, g_nb_wire_fd, 0, now);
}

/* The in-process client transport (the stand-in for libvms's mailboxes). */
struct nb_client { uint32_t unit; };
static int nb_put(void *ctx, const uint8_t *rec, size_t len)
{
    (void)ctx;
    if (g_nbreq_n >= NB_QMAX || len > DNET_BROKER_REQ_MAX) return -1;
    memcpy(g_nbreq[g_nbreq_n].rec, rec, len);
    g_nbreq[g_nbreq_n].len = len;
    g_nbreq_n++;
    return 0;
}
static int nb_get(void *ctx, uint8_t *buf, size_t cap, size_t *len)
{
    return nb_take_reply(((struct nb_client *)ctx)->unit, buf, cap, len);
}
static void nb_idle(void *ctx)
{
    (void)ctx;
    nb_pump();
    struct timespec ts = { 0, 200 * 1000 };   /* let the peer thread run */
    nanosleep(&ts, NULL);
}

/* A remote that refuses every connect with access-control-rejected (34). */
static void *nb_refuser_thread(void *v)
{
    struct copy_wire *w = v;
    for (int i = 0; i < 4096; i++) {
        int ev = w->rxev(w, cw_now(w));
        if (ev < 0) return NULL;
        if (ev == DNET_LINK_EV_CONNECT_IND) {
            uint8_t f[DNET_FRAME_MAX]; size_t fl = 0;
            if (dnet_engine_link_close(w->eng, DNET_LINK_REASON_ACCESS, f, sizeof f, &fl,
                                       cw_now(w)) == DNET_ENGINE_OK)
                (void)w->txf(w, f, fl);
            return NULL;
        }
    }
    return NULL;
}

static int nb_used(void)
{
    int u = 0;
    for (int i = 0; i < NETACP_MAX_SESSIONS; i++) u += g_nbslots[i].used;
    return u;
}

/* Send a raw OPEN request (no waiting) and return its corr id. */
static uint32_t nb_raw_open(uint32_t pid, uint32_t unit, const char *ncb, uint32_t *corr)
{
    struct dnet_broker_req r;
    memset(&r, 0, sizeof r);
    r.corr_id = dnet_broker_corr_next(corr);
    r.owner_pid = pid; r.reply_unit = unit; r.op = DNET_BROKER_OP_OPEN;
    r.datalen = (uint16_t)strlen(ncb);
    memcpy(r.data, ncb, r.datalen);
    uint8_t rec[DNET_BROKER_REQ_MAX]; size_t n = 0;
    dnet_broker_req_encode(&r, rec, sizeof rec, &n);
    nb_put(NULL, rec, n);
    nb_pump();
    return r.corr_id;
}
static uint32_t nb_reply_status(uint32_t unit, uint32_t corr)
{
    uint8_t rec[DNET_BROKER_RSP_MAX]; size_t n = 0;
    struct dnet_broker_rsp rsp;
    while (nb_take_reply(unit, rec, sizeof rec, &n) == 1)
        if (dnet_broker_rsp_decode(rec, n, &rsp) == DNET_BROKER_OK && rsp.corr_id == corr)
            return rsp.status;
    return 0;                              /* no response (yet) */
}

/* A transport whose responses are scripted (the correlation-gate proof). */
struct nb_script { const uint8_t *recs[4]; size_t lens[4]; int n, i; int puts; };
static int nbs_put(void *ctx, const uint8_t *rec, size_t len)
{ (void)rec; (void)len; ((struct nb_script *)ctx)->puts++; return 0; }
static int nbs_get(void *ctx, uint8_t *buf, size_t cap, size_t *len)
{
    struct nb_script *s = ctx;
    if (s->i >= s->n) return 0;
    if (s->lens[s->i] > cap) return -1;
    memcpy(buf, s->recs[s->i], s->lens[s->i]);
    *len = s->lens[s->i];
    s->i++;
    return 1;
}

static int run_netacp_broker_selftest(void)
{
    printf("DECNETD-I-NETACPBROKER, outbound links brokered through NETACP: _NET:"
           " broker records -> NETACP's pool -> NSP link to a peer, correlation-"
           "gated, bounded (no executive, rd vms-dda)\n");
    int pass = 0, fail = 0;
#define NA_CHECK(c, msg) do { if (c) { pass++; printf("  PASS: %s\n", msg); } \
    else { fail++; printf("  FAIL: %s\n", msg); } } while (0)

    const uint8_t hwA[6] = { 0x02,0,0,0,0,0x0a };
    const uint8_t hwB[6] = { 0x02,0,0,0,0,0x0b };
    static struct dnet_engine B;
    dnet_engine_init(&g_nbnode, 1, 10, "OVMXA", "EWA0", NULL, hwA, 0, 0, 0);
    memcpy(g_netacp_self, g_nbnode.my_id, 6);
    g_netacp_self_set = 1;
    g_netacp_tx = nb_tx;
    g_netacp_reply = nb_reply_capture;
    memset(g_nbslots, 0, sizeof g_nbslots);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        fprintf(stderr, "DECNETD-E-NETACPBROKER, socketpair failed: %s\n", strerror(errno));
        return 1;
    }
    g_nb_wire_fd = sv[0];

    struct nb_client cl = { 0x51 };
    struct dnet_broker_io io = { &cl, nb_put, nb_get, nb_idle, 2000, 20000 };
    struct dnet_broker_chan bc;
    memset(&bc, 0, sizeof bc);
    bc.owner_pid = 0;                       /* no executive: NETACP's own identity */
    bc.reply_unit = cl.unit;
    struct netcli nc = { 0, &bc, &io };

    /* (1) $ COPY through NETACP to a FAL peer on 1.11 (test double, no auth). */
    {
        struct dnet_copy_plan plan;
        int rp = dnet_copy_plan("1.11::DKA0:[X]NOPE.TXT", "DKA0:[X]LOCAL.TXT", &plan);
        dnet_engine_init(&B, 1, 11, "OVMXB", "EWA0", NULL, hwB, 0, 0, 0);
        struct copy_server_arg sarg;
        memset(&sarg, 0, sizeof sarg);
        sarg.w.eng = &B; sarg.w.txf = cw_sp_tx; sarg.w.rxev = cw_sp_rx;
        sarg.w.sp_wfd = sv[1]; sarg.w.sp_rfd = sv[1]; sarg.w.synthetic = 1; sarg.w.clk = 1000;
        pthread_t th;
        int thr = (rp == 0) ? pthread_create(&th, NULL, copy_noauth_server_thread, &sarg) : -1;
        uint32_t cs = SS$_ABORT;
        unsigned long wire0 = g_nb_wire;
        if (thr == 0) {
            cs = copy_client_run_net(&nc, &plan, "");
            pthread_join(th, NULL);
        }
        NA_CHECK(rp == 0 && thr == 0 && cs == SS$_NOSUCHFILE && sarg.status == SS$_NOSUCHFILE,
                 "COPY's FAL client opened an object-17 link THROUGH NETACP (broker OPEN completed on the"
                 " remote's Connect Confirm), drove a DAP session both ways over SEND/RECV, and the honest"
                 " miss round-trips (client + server both NOSUCHFILE)");
        NA_CHECK(g_nb_wire > wire0 && bc.handle == 0 && nb_used() == 0,
                 "the frames went out on NETACP's datalink; DEACCESS disconnected the link and freed its pool slot");
    }

    /* (2) A remote that rejects the access control: OPEN completes INVLOGIN. */
    {
        dnet_engine_init(&B, 1, 11, "OVMXB", "EWA0", NULL, hwB, 0, 0, 0);
        struct copy_wire w;
        memset(&w, 0, sizeof w);
        w.eng = &B; w.txf = cw_sp_tx; w.rxev = cw_sp_rx;
        w.sp_wfd = sv[1]; w.sp_rfd = sv[1]; w.synthetic = 1; w.clk = 2000;
        pthread_t th;
        uint32_t st = SS$_ABORT;
        if (pthread_create(&th, NULL, nb_refuser_thread, &w) == 0) {
            const char *ncb = "1.11\"GUEST WRONGPW\"::\"17=\"";
            size_t x = 0;
            st = netcli_op(&nc, DNET_BROKER_OP_OPEN, ncb, strlen(ncb), NULL, 0, &x);
            pthread_join(th, NULL);
        }
        NA_CHECK(st == SS$_INVLOGIN && bc.handle == 0 && nb_used() == 0,
                 "a connect the remote refuses with access-control-rejected completes the OPEN as"
                 " SS$_INVLOGIN, with no link and no slot left behind");
    }

    /* (3) This node to ITSELF (0::) -- NETACP's local loopback reaches its own
     * inbound dispatch, which serves no object named NOSUCHTASK. */
    {
        unsigned long wire0 = g_nb_wire;
        const char *ncb = "0::\"TASK=NOSUCHTASK\"";
        size_t x = 0;
        uint32_t st = netcli_op(&nc, DNET_BROKER_OP_OPEN, ncb, strlen(ncb), NULL, 0, &x);
        NA_CHECK(st == SS$_ABORT && g_nb_wire == wire0 && nb_used() == 0,
                 "a link to this node itself (0::) runs over NETACP's local loopback -- never the wire --"
                 " and its own inbound dispatch refuses the unserved object honestly (ABORT)");
    }

    /* (3b) This node to ITSELF with credentials its inbound FAL refuses: the
     * loopback refusal must reach the outbound slot, which completes the OPEN
     * SS$_INVLOGIN -- never a client left waiting (the booted bad-password COPY). */
    {
        unsigned long wire0 = g_nb_wire;
        const char *ncb = "0\"SYSTEM WRONGPW\"::\"17=\"";
        size_t x = 0;
        uint32_t st = netcli_op(&nc, DNET_BROKER_OP_OPEN, ncb, strlen(ncb), NULL, 0, &x);
        printf("  INFO: loopback refused-FAL OPEN -> %08X\n", (unsigned)st);
        NA_CHECK(st == SS$_INVLOGIN && g_nb_wire == wire0 && nb_used() == 0,
                 "a link to this node itself whose credentials its own FAL refuses completes the OPEN"
                 " SS$_INVLOGIN over the loopback, with no slot left behind");
    }

    /* (4) Honest refusals before any link is attempted. */
    {
        size_t x = 0;
        const char *bad = "NOT AN NCB";
        uint32_t st = netcli_op(&nc, DNET_BROKER_OP_OPEN, bad, strlen(bad), NULL, 0, &x);
        NA_CHECK(st == SS$_BADPARAM, "a malformed NCB is refused SS$_BADPARAM");
        const char *nosuch = "NOSUCHNODE::\"17=\"";
        st = netcli_op(&nc, DNET_BROKER_OP_OPEN, nosuch, strlen(nosuch), NULL, 0, &x);
        NA_CHECK(st == SS$_NOSUCHDEV, "an unresolvable node is refused (no address invented)");
        uint8_t b[16];
        st = netcli_op(&nc, DNET_BROKER_OP_RECV, NULL, 0, b, sizeof b, &x);
        NA_CHECK(st == SS$_FILNOTACC, "READVBLK on a channel with no link is SS$_FILNOTACC");
    }

    /* (5) BOUNDS: a process over its share, then a full pool. Opens to 1.11
     * stay pending (nobody answers); the wire is discarded. */
    {
        g_nb_wire_fd = -1;
        uint32_t corr = 0;
        uint32_t cs[NETACP_MAX_PER_SOURCE];
        for (int i = 0; i < NETACP_MAX_PER_SOURCE; i++)
            cs[i] = nb_raw_open(0x100, 0x61, "1.11::\"17=\"", &corr);
        unsigned long wire0 = g_nb_wire;
        uint32_t cx = nb_raw_open(0x100, 0x61, "1.11::\"17=\"", &corr);
        uint32_t sx = nb_reply_status(0x61, cx);   /* consumes unit 0x61's queue */
        int pending = 1;
        for (int i = 0; i < NETACP_MAX_PER_SOURCE; i++)
            pending = pending && nb_reply_status(0x61, cs[i]) == 0;
        NA_CHECK(sx == SS$_EXQUOTA && pending && g_nb_wire == wire0,
                 "a process holding its share of links is refused another (EXQUOTA), no Connect Initiate sent");
        /* Fill the rest of the pool, each process taking at most its share. */
        int left = NETACP_MAX_SESSIONS - NETACP_MAX_PER_SOURCE;
        for (uint32_t p = 0x200; left > 0; p++)
            for (int i = 0; i < NETACP_MAX_PER_SOURCE && left > 0; i++, left--)
                nb_raw_open(p, 0x62, "1.11::\"17=\"", &corr);
        int full = nb_used();
        wire0 = g_nb_wire;
        uint32_t cf = nb_raw_open(0x300, 0x63, "1.11::\"17=\"", &corr);
        NA_CHECK(full == NETACP_MAX_SESSIONS && nb_reply_status(0x63, cf) == SS$_EXQUOTA &&
                 g_nb_wire == wire0,
                 "the pool holds NETACP_MAX_SESSIONS links and a fresh process is refused once it is full -- bounded");
        /* A request naming another process's link is refused. */
        uint32_t h = 0;
        for (int i = 0; i < NETACP_MAX_SESSIONS; i++)
            if (g_nbslots[i].used && g_nbslots[i].owner_pid == 0x100) h = g_nbslots[i].handle;
        struct dnet_broker_req r;
        memset(&r, 0, sizeof r);
        r.corr_id = dnet_broker_corr_next(&corr);
        r.owner_pid = 0x300; r.reply_unit = 0x63; r.link_handle = h;
        r.op = DNET_BROKER_OP_RECV;
        uint8_t rec[DNET_BROKER_REQ_MAX]; size_t n = 0;
        dnet_broker_req_encode(&r, rec, sizeof rec, &n);
        nb_put(NULL, rec, n);
        nb_pump();
        NA_CHECK(h != 0 && nb_reply_status(0x63, r.corr_id) == SS$_FILNOTACC,
                 "a request naming ANOTHER process's link handle is refused (FILNOTACC) -- never served");
        g_nbrep_n = 0;
        for (int i = 0; i < NETACP_MAX_SESSIONS; i++)
            netacp_slot_end(&g_nbslots[i], -1, 0, monotonic_sec(), "selftest teardown");
        NA_CHECK(nb_used() == 0 && g_nbrep_n == NETACP_MAX_SESSIONS,
                 "teardown frees every slot and answers each pending OPEN (no client left waiting)");
        g_nbrep_n = 0;
    }

    /* (6) CORRELATION: the client delivers only the matching response. */
    {
        struct dnet_broker_req q;
        memset(&q, 0, sizeof q);
        q.corr_id = 77; q.op = DNET_BROKER_OP_RECV;
        struct dnet_broker_rsp a, b2;
        memset(&a, 0, sizeof a); memset(&b2, 0, sizeof b2);
        a.corr_id = 76; a.status = SS$_NORMAL; a.datalen = 5; memcpy(a.data, "STALE", 5);
        b2.corr_id = 77; b2.status = SS$_NORMAL; b2.datalen = 4; memcpy(b2.data, "MINE", 4);
        uint8_t ra[DNET_BROKER_RSP_MAX], rb[DNET_BROKER_RSP_MAX], junk[9] = { 0x52,0x54,0x45,0x4e, 1,2,3 };
        size_t la = 0, lb = 0;
        dnet_broker_rsp_encode(&a, ra, sizeof ra, &la);
        dnet_broker_rsp_encode(&b2, rb, sizeof rb, &lb);
        struct nb_script sc = { { ra, junk, rb }, { la, sizeof junk, lb }, 3, 0, 0 };
        struct dnet_broker_io sio = { &sc, nbs_put, nbs_get, NULL, 10, 10 };
        struct dnet_broker_rsp got;
        uint32_t mism = 0;
        int r = dnet_broker_call(&sio, &q, &got, 10, &mism);
        NA_CHECK(r == DNET_BROKER_OK && got.corr_id == 77 && got.datalen == 4 &&
                 memcmp(got.data, "MINE", 4) == 0 && mism == 2,
                 "a stale (wrong correlation id) and a malformed response are DROPPED; only the matching"
                 " response is delivered to the waiting request");
        struct nb_script none = { { 0 }, { 0 }, 0, 0, 0 };
        struct dnet_broker_io dio = { &none, nbs_put, nbs_get, NULL, 5, 5 };
        struct dnet_broker_chan dbc;
        memset(&dbc, 0, sizeof dbc);
        size_t x = 0;
        const char *ncb = "1.11::\"17=\"";
        uint32_t st = dnet_broker_xfer(&dbc, &dio, DNET_BROKER_OP_OPEN, ncb, strlen(ncb),
                                       NULL, 0, &x);
        NA_CHECK(st == SS$_DEVOFFLINE && none.puts == 1 && dbc.handle == 0,
                 "a NETACP that never answers completes the $QIO SS$_DEVOFFLINE -- never a fake success");
    }

    g_nb_wire_fd = -1;
    g_netacp_tx = scs_datalink_send;
    g_netacp_reply = netacp_reply_mbx;
    g_netacp_self_set = 0;
    close(sv[0]); close(sv[1]);
    printf("DECNETD-I-NETACPBROKER, %d passed, %d failed\n", pass, fail);
    if (fail == 0 && pass > 0) { printf("DECNETD-NETACP-BROKER-SELFTEST: PASS\n"); return 0; }
    printf("DECNETD-NETACP-BROKER-SELFTEST: FAIL\n");
    return 1;
#undef NA_CHECK
}

/*
 * ===================== --netacp-show-selftest (rd vms-30e) =====================
 * The HOST FLOOR of NCP SHOW / SHOW NETWORK read from the running NETACP. No
 * executive. It proves:
 *   A. the snapshot record codec: every entity round-trips; every truncated
 *      prefix, an over-bound count / string length, trailing bytes, a bad
 *      version or entity and a control byte in a name are REFUSED; 200k random
 *      mutations never over-read (ASan/UBSan in the sanitizer leg) and anything
 *      that does decode is in bounds and printable; the query codec likewise;
 *   B. NETACP SERVES a SHOW from its live state: a real NETACP pool (the code
 *      the daemon runs) on node 1.10 with a node database on disk, an adjacency
 *      learned from a real endnode hello, one outbound link that completed its
 *      NSP handshake with a peer engine across a socketpair and one still
 *      connecting -- reached ONLY through broker records (dnet_broker_control,
 *      what libvms's IO$_ACPCONTROL runs) and gathered by the client NCP and
 *      DCL use (dnet_netshow_fetch). Every number checked is one the test made
 *      true in NETACP's state; a malformed query is refused BADPARAM; a silent
 *      NETACP is DEVOFFLINE;
 *   C. the formatters reproduce the REAL OpenVMS VAX V7.3 oracle
 *      (docs/oracle/vax-ncp-show/) byte for byte when fed the oracle node's
 *      values -- the "as of" timestamp normalized; for CHARACTERISTICS and
 *      COUNTERS, which print only what NETACP keeps, every OVMX line is a line
 *      of the oracle, in the oracle's order.
 */
static const char *g_show_oracle_dir;

struct show_lines { char l[64][260]; int n; };
static void show_collect(void *ctx, const char *line)
{
    struct show_lines *s = ctx;
    if (s->n < 64)
        snprintf(s->l[s->n++], sizeof s->l[0], "%s", line);
}
/* "... as of <anything>" -> "... as of <T>" (the dynamic field). */
static void show_norm(char *line)
{
    char *p = strstr(line, " as of ");
    if (p)
        strcpy(p + 7, "<T>");
}
/* Load an oracle transcript: drop the echoed command (first line) and the
 * trailing DCL prompt ("$"); CRs stripped; "as of" normalized. */
static int show_oracle(const char *file, struct show_lines *o)
{
    char path[512];
    memset(o, 0, sizeof *o);
    snprintf(path, sizeof path, "%s/%s", g_show_oracle_dir ? g_show_oracle_dir : ".", file);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char buf[256];
    int first = 1;
    while (fgets(buf, sizeof buf, f) && o->n < 64) {
        buf[strcspn(buf, "\r\n")] = '\0';
        if (first) { first = 0; continue; }
        snprintf(o->l[o->n++], sizeof o->l[0], "%s", buf);
    }
    fclose(f);
    if (o->n > 0 && strcmp(o->l[o->n - 1], "$") == 0)
        o->n--;
    for (int i = 0; i < o->n; i++)
        show_norm(o->l[i]);
    return 0;
}
static int show_equal(struct show_lines *a, struct show_lines *b, const char *what)
{
    for (int i = 0; i < a->n; i++)
        show_norm(a->l[i]);
    if (a->n != b->n) {
        printf("    %s: %d lines vs the oracle's %d\n", what, a->n, b->n);
        return 0;
    }
    for (int i = 0; i < a->n; i++)
        if (strcmp(a->l[i], b->l[i]) != 0) {
            printf("    %s line %d:\n      ovmx  : '%s'\n      oracle: '%s'\n", what, i,
                   a->l[i], b->l[i]);
            return 0;
        }
    return 1;
}
/* Every OVMX line is an oracle line, in order; the first `head` lines and the
 * last `tail` lines match exactly. */
static int show_subseq(struct show_lines *a, struct show_lines *b, int head, int tail,
                       const char *what)
{
    for (int i = 0; i < a->n; i++)
        show_norm(a->l[i]);
    if (a->n < head + tail || b->n < head + tail)
        return 0;
    for (int i = 0; i < head; i++)
        if (strcmp(a->l[i], b->l[i]) != 0) {
            printf("    %s head line %d: '%s' vs '%s'\n", what, i, a->l[i], b->l[i]);
            return 0;
        }
    for (int i = 1; i <= tail; i++)
        if (strcmp(a->l[a->n - i], b->l[b->n - i]) != 0)
            return 0;
    int j = head;
    for (int i = head; i < a->n - tail; i++) {
        while (j < b->n - tail && strcmp(a->l[i], b->l[j]) != 0)
            j++;
        if (j >= b->n - tail) {
            printf("    %s: '%s' is not an oracle line (or out of order)\n", what, a->l[i]);
            return 0;
        }
        j++;
    }
    return 1;
}

/* The client transport onto the in-process NETACP (broker records). */
struct show_cli { struct dnet_broker_chan *bc; const struct dnet_broker_io *io; };
static uint32_t show_cli_query(void *ctx, const uint8_t *req, size_t reqlen,
                               uint8_t *rsp, size_t rspcap, size_t *rsplen)
{
    struct show_cli *c = ctx;
    return dnet_broker_control(c->bc, c->io, req, reqlen, rsp, rspcap, rsplen);
}
/* A query whose answer is another entity's record (the client must refuse). */
static uint32_t show_wrong_entity(void *ctx, const uint8_t *req, size_t reqlen,
                                  uint8_t *rsp, size_t rspcap, size_t *rsplen)
{
    (void)ctx; (void)req; (void)reqlen;
    static struct dnet_netshow_rsp r;
    memset(&r, 0, sizeof r);
    r.entity = DNET_NETSHOW_ENT_LINKS;
    return dnet_netshow_rsp_encode(&r, rsp, rspcap, rsplen) == DNET_NETSHOW_OK ? 1u : 20u;
}
static int nbs_silent_get(void *ctx, uint8_t *buf, size_t cap, size_t *len)
{ (void)ctx; (void)buf; (void)cap; (void)len; return 0; }

static int show_printable(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7e)
            return 0;
    return 1;
}

static int run_netacp_show_selftest(void)
{
    printf("DECNETD-I-NETACPSHOW, NCP SHOW / SHOW NETWORK read the running NETACP: snapshot"
           " codec, NETACP serving from its live state, oracle layout (no executive,"
           " rd vms-30e)\n");
    int pass = 0, fail = 0;
#define NS2_CHECK(c, msg) do { if (c) { pass++; printf("  PASS: %s\n", msg); } \
    else { fail++; printf("  FAIL: %s\n", msg); } } while (0)

    static struct dnet_netshow_rsp r, d;
    static struct dnet_netshow_view v;
    uint8_t rec[DNET_NETSHOW_RSP_MAX + 64], fz[DNET_NETSHOW_RSP_MAX + 64];
    size_t n = 0;

    /* ---------------- A. the record codec ---------------- */
    {
        memset(&r, 0, sizeof r);
        r.entity = DNET_NETSHOW_ENT_NODES;
        r.total = 5; r.first = 2; r.count = 3; r.as_of = 1791148795u;
        r.exec.addr = (1u << 10) | 1u; r.exec.state_on = 1;
        snprintf(r.exec.name, sizeof r.exec.name, "VAX1");
        snprintf(r.exec.ident, sizeof r.exec.ident, "a test identification");
        snprintf(r.exec.circuit, sizeof r.exec.circuit, "QNA-0");
        r.exec.nsp_ver[0] = 4; r.exec.nsp_ver[1] = 1;
        r.exec.rtg_ver[0] = 2; r.exec.max_links = 32; r.exec.max_links_active = 3;
        for (int i = 0; i < 3; i++) {
            r.u.node[i].addr = (uint16_t)((1u << 10) | (2u + (unsigned)i));
            snprintf(r.u.node[i].name, sizeof r.u.node[i].name, "N%d", i);
            r.u.node[i].flags = DNET_NETSHOW_NF_INDB;
            r.u.node[i].active_links = (uint16_t)i;
        }
        int e = dnet_netshow_rsp_encode(&r, rec, sizeof rec, &n);
        int dd = e == DNET_NETSHOW_OK ? dnet_netshow_rsp_decode(rec, n, &d) : -99;
        NS2_CHECK(e == DNET_NETSHOW_OK && dd == DNET_NETSHOW_OK && n ==
                  DNET_NETSHOW_HDR_LEN + DNET_NETSHOW_EXEC_LEN + 3 * DNET_NETSHOW_NODE_LEN &&
                  d.total == 5 && d.first == 2 && d.count == 3 && d.as_of == r.as_of &&
                  !strcmp(d.exec.ident, r.exec.ident) && !strcmp(d.exec.circuit, "QNA-0") &&
                  d.exec.max_links_active == 3 && !strcmp(d.u.node[2].name, "N2") &&
                  d.u.node[2].active_links == 2,
                  "a NODES snapshot record round-trips field for field");

        int all_refused = 1;
        for (size_t k = 0; k < n; k++)
            if (dnet_netshow_rsp_decode(rec, k, &d) == DNET_NETSHOW_OK) all_refused = 0;
        NS2_CHECK(all_refused, "every truncated prefix of the record is refused (never an over-read)");

        memcpy(fz, rec, n); fz[n] = 0;
        NS2_CHECK(dnet_netshow_rsp_decode(fz, n + 1, &d) == DNET_NETSHOW_EBADLEN,
                  "a record with trailing bytes is refused");
        memcpy(fz, rec, n); fz[0] = 2;
        NS2_CHECK(dnet_netshow_rsp_decode(fz, n, &d) == DNET_NETSHOW_EVERS,
                  "an unknown record version is refused");
        memcpy(fz, rec, n); fz[1] = 9;
        NS2_CHECK(dnet_netshow_rsp_decode(fz, n, &d) == DNET_NETSHOW_EINVAL,
                  "an unknown entity is refused");
        memcpy(fz, rec, n); fz[6] = 200;
        NS2_CHECK(dnet_netshow_rsp_decode(fz, n, &d) == DNET_NETSHOW_EBADLEN,
                  "an entry count over the page bound is refused");
        memcpy(fz, rec, n); fz[2] = 4; fz[3] = 0;      /* total 4 < first 2 + count 3 */
        NS2_CHECK(dnet_netshow_rsp_decode(fz, n, &d) == DNET_NETSHOW_EBADLEN,
                  "a page that runs past the table total is refused");
        memcpy(fz, rec, n); fz[DNET_NETSHOW_HDR_LEN + 4] = 7;   /* exec name length */
        NS2_CHECK(dnet_netshow_rsp_decode(fz, n, &d) == DNET_NETSHOW_EBADLEN,
                  "a string length over its slot is refused");
        memcpy(fz, rec, n); fz[DNET_NETSHOW_HDR_LEN + 5] = 0x1b;  /* ESC in the name */
        NS2_CHECK(dnet_netshow_rsp_decode(fz, n, &d) == DNET_NETSHOW_EINVAL &&
                  d.count == 0 && d.exec.name[0] == '\0',
                  "a control byte in a name is refused, and *out is left zeroed");

        uint32_t seed = 0x30eu;
        unsigned long decoded = 0, bad = 0;
        for (int it = 0; it < 200000; it++) {
            size_t ln = n;
            memcpy(fz, rec, n);
            int flips = 1 + (int)(nbself_rand(&seed) % 4);
            for (int k = 0; k < flips; k++)
                fz[nbself_rand(&seed) % n] = (uint8_t)nbself_rand(&seed);
            if (nbself_rand(&seed) % 8 == 0)
                ln = nbself_rand(&seed) % (n + 1);
            if (dnet_netshow_rsp_decode(fz, ln, &d) == DNET_NETSHOW_OK) {
                decoded++;
                if (d.count > DNET_NETSHOW_NODES_PER || (uint32_t)d.first + d.count > d.total ||
                    !show_printable(d.exec.name) || !show_printable(d.exec.ident) ||
                    !show_printable(d.exec.circuit) || strlen(d.exec.name) > DNET_NETSHOW_NAMEMAX)
                    bad++;
                for (unsigned k = 0; k < d.count && d.entity == DNET_NETSHOW_ENT_NODES; k++)
                    if (!show_printable(d.u.node[k].name)) bad++;
            }
        }
        printf("    (fuzz: 200000 mutations, %lu still decoded, all bounds-checked)\n", decoded);
        NS2_CHECK(bad == 0, "200k random mutations: nothing over-reads, and every record that"
                            " still decodes is in bounds with printable strings");

        memset(&r, 0, sizeof r);
        r.entity = DNET_NETSHOW_ENT_LINKS;
        r.total = 1; r.count = 1;
        r.u.link[0].local_link = 0x2101; r.u.link[0].remote_link = 0x0c05;
        r.u.link[0].node = (1u << 10) | 2u; r.u.link[0].pid = 0x20200216u;
        r.u.link[0].object = 42; r.u.link[0].running = 1;
        snprintf(r.u.link[0].name, sizeof r.u.link[0].name, "VAX2");
        e = dnet_netshow_rsp_encode(&r, rec, sizeof rec, &n);
        dd = e == DNET_NETSHOW_OK ? dnet_netshow_rsp_decode(rec, n, &d) : -99;
        NS2_CHECK(dd == DNET_NETSHOW_OK && d.u.link[0].pid == 0x20200216u &&
                  d.u.link[0].remote_link == 0x0c05 && d.u.link[0].object == 42 &&
                  d.u.link[0].running == 1 && !strcmp(d.u.link[0].name, "VAX2"),
                  "a LINKS snapshot record round-trips field for field");
        memset(r.exec.name, 'X', sizeof r.exec.name);           /* 7 chars, no NUL */
        NS2_CHECK(dnet_netshow_rsp_encode(&r, rec, sizeof rec, &n) == DNET_NETSHOW_EBADLEN,
                  "the encoder refuses an over-bound string instead of truncating it");

        struct dnet_netshow_req q = { DNET_NETSHOW_VERSION, DNET_NETSHOW_ENT_LINKS, 7 }, qd;
        uint8_t qb[8];
        size_t qn = 0;
        int qe = dnet_netshow_req_encode(&q, qb, sizeof qb, &qn);
        NS2_CHECK(qe == DNET_NETSHOW_OK && dnet_netshow_req_decode(qb, qn, &qd) == DNET_NETSHOW_OK &&
                  qd.entity == DNET_NETSHOW_ENT_LINKS && qd.cursor == 7 &&
                  dnet_netshow_req_decode(qb, qn - 1, &qd) == DNET_NETSHOW_ETRUNC &&
                  dnet_netshow_req_decode(qb, qn + 1, &qd) == DNET_NETSHOW_EBADLEN,
                  "the query round-trips; a short or long query is refused");
        qb[1] = 0;
        NS2_CHECK(dnet_netshow_req_decode(qb, qn, &qd) == DNET_NETSHOW_EINVAL,
                  "a query for an unknown entity is refused");
        NS2_CHECK(dnet_netshow_fetch(show_wrong_entity, NULL, DNET_NETSHOW_ENT_NODES, &v) ==
                  DNET_NETSHOW_ST_BADREC,
                  "the client refuses an answer that is not the entity it asked for");
    }

    /* ---------------- B. NETACP serves the SHOW from its live state ---------------- */
    char dbpath[] = "/tmp/dnshow-nodedbXXXXXX";
    int dbfd = mkstemp(dbpath);
    if (dbfd >= 0) close(dbfd);
    setenv("OVMX_DECNET_NODEDB", dbpath, 1);
    {
        static struct dnet_nodedb db;
        dnet_nodedb_init(&db);
        dnet_nodedb_set(&db, (1u << 10) | 2u, "VAX2");
        dnet_nodedb_set(&db, (1u << 10) | 5u, "");
        dnet_nodedb_set(&db, (1u << 10) | 11u, "VAXB");
        NS2_CHECK(dbfd >= 0 && dnet_store_save_nodes(&db) == DNET_STORE_OK,
                  "NETACP's node database holds 1.2 VAX2, 1.5 and 1.11 VAXB (the file it resolves by)");
    }
    const uint8_t hwA[6] = { 0x02,0,0,0,0,0x0a };
    const uint8_t hwB[6] = { 0x02,0,0,0,0,0x0b };
    static struct dnet_engine B;
    dnet_engine_init(&g_nbnode, 1, 10, "OVMXA", "EWA0", NULL, hwA, 0, 0, 0);
    dnet_engine_init(&B, 1, 11, "VAXB", "EWA0", NULL, hwB, 0, 0, 0);
    memcpy(g_netacp_self, g_nbnode.my_id, 6);
    g_netacp_self_set = 1;
    g_netacp_tx = nb_tx;
    g_netacp_reply = nb_reply_capture;
    g_netacp_links_hwm = 0;
    memset(g_nbslots, 0, sizeof g_nbslots);
    g_nbreq_n = g_nbrep_n = 0;

    struct nb_client cl = { 0x52 };
    struct dnet_broker_io io = { &cl, nb_put, nb_get, nb_idle, 2000, 20000 };
    struct dnet_broker_chan bc;
    memset(&bc, 0, sizeof bc);
    bc.reply_unit = cl.unit;
    struct show_cli sc = { &bc, &io };

    /* Live state 1: the adjacency -- B's real endnode hello, received. */
    {
        uint8_t f[DNET_FRAME_MAX], from[6];
        size_t fl = 0;
        enum dnet_adj_state ast = DNET_ADJ_DOWN;
        int hb = dnet_engine_build_hello_frame(&B, f, sizeof f, &fl);
        int rx = hb == DNET_ENGINE_OK ? dnet_engine_rx_frame(&g_nbnode, 5, f, fl, from, &ast) : -1;
        NS2_CHECK(rx == 1 && dnet_adj_state_of(&g_nbnode.adj, B.my_id) != DNET_ADJ_DOWN,
                  "NETACP heard 1.11's endnode hello: an adjacency exists in its table");
    }
    /* Live state 2: an outbound link that RUNS -- a real NSP handshake with B. */
    int sv[2] = { -1, -1 };
    uint32_t corr = 0;
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0) {
        g_nb_wire_fd = sv[0];
        uint32_t c1 = nb_raw_open(0x20200212u, 0x61, "1.11::\"42=\"", &corr);
        uint8_t f[DNET_FRAME_MAX], rep[DNET_FRAME_MAX];
        ssize_t fl = recv(sv[1], f, sizeof f, MSG_DONTWAIT);
        size_t rl = 0, al = 0;
        int has = 0;
        enum dnet_link_event ev = DNET_LINK_EV_NONE;
        B.link_active = 0;
        int lr = fl > 0 ? dnet_engine_link_rx(&B, 6, f, (size_t)fl, rep, sizeof rep, &rl,
                                              &has, &ev) : -1;
        int ac = (lr == 0 && ev == DNET_LINK_EV_CONNECT_IND)
                 ? dnet_engine_link_accept(&B, 0x0c05, rep, sizeof rep, &al, 6) : -1;
        if (ac == 0)
            (void)write(sv[1], rep, al);
        nb_pump();
        NS2_CHECK(ac == 0 && nb_reply_status(0x61, c1) == SS$_NORMAL,
                  "process 20200212's outbound link to 1.11 object 42 completed its NSP"
                  " handshake through NETACP (OPEN answered on the remote's Connect Confirm)");
    }
    /* Live state 3: a second link still connecting (nobody answers). */
    g_nb_wire_fd = -1;
    (void)nb_raw_open(0x20200213u, 0x62, "1.2::\"17=\"", &corr);
    NS2_CHECK(nb_used() == 2, "NETACP's pool holds the two links");
    /* Close the connecting one: the high-water mark must stay 2, active go to 1. */
    {
        int closed = 0;
        for (int i = 0; i < NETACP_MAX_SESSIONS; i++)
            if (g_nbslots[i].used && g_nbslots[i].owner_pid == 0x20200213u) {
                netacp_slot_end(&g_nbslots[i], -1, 0, monotonic_sec(), "selftest");
                closed = 1;
            }
        NS2_CHECK(closed && nb_used() == 1, "the connecting link is released (one link left)");
        (void)nb_raw_open(0x20200213u, 0x62, "1.2::\"17=\"", &corr);   /* again: connecting */
    }

    /* The SHOW, through broker records only. */
    uint32_t st = dnet_netshow_fetch(show_cli_query, &sc, DNET_NETSHOW_ENT_EXECUTOR, &v);
    const struct dnet_netshow_exec *x = &v.exec;
    NS2_CHECK(st == 1 && x->addr == ((1u << 10) | 10u) && !strcmp(x->name, "OVMXA") &&
              x->state_on && x->type == DNET_NETSHOW_TYPE_NONROUTING &&
              !strcmp(x->circuit, "EWA-0") && !strncmp(x->ident, "OVMX DECnet-compatible", 22),
              "SHOW EXECUTOR: NETACP answers its own address, name, state, type, circuit and"
              " OVMX identification (read from its engine, not a constant)");
    NS2_CHECK(x->max_links == NETACP_MAX_SESSIONS && x->active_links == 2 &&
              x->max_links_active == 2 && x->nsp_ver[0] == 4 && x->nsp_ver[1] == 1 &&
              x->rtg_ver[0] == DNET_ENGINE_ROUTING_VERSION,
              "CHARACTERISTICS/COUNTERS: pool size, links in use (2) and the high-water"
              " counter (2) are NETACP's own; the NSP version is what its CIs carry, the routing version what its hellos carry");
    NS2_CHECK(v.as_of != 0 && v.nnodes == 0 && v.nlinks == 0,
              "an EXECUTOR query carries the executor block only");

    st = dnet_netshow_fetch(show_cli_query, &sc, DNET_NETSHOW_ENT_NODES, &v);
    int ok_nodes = st == 1 && v.nnodes == 3 &&
        v.nodes[0].addr == ((1u << 10) | 2u) && !strcmp(v.nodes[0].name, "VAX2") &&
        v.nodes[0].active_links == 1 && v.nodes[0].flags == DNET_NETSHOW_NF_INDB &&
        v.nodes[1].addr == ((1u << 10) | 5u) && v.nodes[1].name[0] == '\0' &&
        v.nodes[1].active_links == 0 &&
        v.nodes[2].addr == ((1u << 10) | 11u) && !strcmp(v.nodes[2].name, "VAXB") &&
        v.nodes[2].active_links == 1 &&
        v.nodes[2].flags == (DNET_NETSHOW_NF_INDB | DNET_NETSHOW_NF_ADJ) &&
        v.nodes[2].adj_state != DNET_NETSHOW_ADJ_NONE && v.nodes[2].next_node == 0;
    NS2_CHECK(ok_nodes, "SHOW KNOWN NODES: the database's nodes in address order, 1.11 also in"
                        " the adjacency table, each with the links NETACP holds to it now;"
                        " next node 0 (no designated router) -- the executor not listed");

    st = dnet_netshow_fetch(show_cli_query, &sc, DNET_NETSHOW_ENT_LINKS, &v);
    int run = -1, conn = -1;
    for (unsigned i = 0; i < v.nlinks; i++) {
        if (v.links[i].pid == 0x20200212u) run = (int)i;
        if (v.links[i].pid == 0x20200213u) conn = (int)i;
    }
    NS2_CHECK(st == 1 && v.nlinks == 2 && run >= 0 && conn >= 0 &&
              v.links[run].running == 1 && v.links[run].remote_link == 0x0c05 &&
              v.links[run].object == 42 && v.links[run].outbound == 1 &&
              !strcmp(v.links[run].name, "VAXB") && v.links[run].local_link >= 0x2100 &&
              v.links[conn].running == 0 && v.links[conn].object == 17 &&
              v.links[conn].node == ((1u << 10) | 2u),
              "SHOW KNOWN LINKS: both links of the pool, with their process, local and remote"
              " link addresses, object and state (running / connecting) as NETACP holds them");

    /* Paging: a pool larger than a page is walked by cursor. */
    {
        struct dnet_netshow_req q = { DNET_NETSHOW_VERSION, DNET_NETSHOW_ENT_NODES, 1 };
        uint8_t qb[8], out[DNET_NETSHOW_RSP_MAX + 16];
        size_t qn = 0, on = 0;
        dnet_netshow_req_encode(&q, qb, sizeof qb, &qn);
        uint32_t ps = dnet_broker_control(&bc, &io, qb, qn, out, sizeof out, &on);
        NS2_CHECK((ps & 1) && dnet_netshow_rsp_decode(out, on, &d) == DNET_NETSHOW_OK &&
                  d.first == 1 && d.count == 2 && d.total == 3 &&
                  d.u.node[0].addr == ((1u << 10) | 5u),
                  "a query with cursor 1 answers the table from its second entry (page walk)");
        uint8_t junk[3] = { 1, 2, 3 };
        ps = dnet_broker_control(&bc, &io, junk, sizeof junk, out, sizeof out, &on);
        NS2_CHECK(ps == SS$_BADPARAM && on == 0,
                  "NETACP refuses a malformed query SS$_BADPARAM, with no record");
        ps = dnet_broker_control(&bc, &io, qb, qn, out, 10, &on);
        NS2_CHECK(ps == SS$_BUFFEROVF && on == 10,
                  "an answer longer than the caller's buffer completes SS$_BUFFEROVF (never an overrun)");
        struct dnet_broker_io silent = { NULL, nbs_put, nbs_silent_get, NULL, 3, 3 };
        struct nb_script scr;
        memset(&scr, 0, sizeof scr);
        silent.ctx = &scr;
        ps = dnet_broker_control(&bc, &silent, qb, qn, out, sizeof out, &on);
        NS2_CHECK(ps == SS$_DEVOFFLINE && on == 0,
                  "a NETACP that does not answer is SS$_DEVOFFLINE -- no snapshot is invented");
    }

    /* The live view through the formatters. */
    {
        static struct show_lines s;
        memset(&s, 0, sizeof s);
        (void)dnet_netshow_fetch(show_cli_query, &sc, DNET_NETSHOW_ENT_NODES, &v);
        dnet_netshow_fmt_known_nodes(&v, show_collect, &s);
        int found_row = 0, found_exec = 0;
        for (int i = 0; i < s.n; i++) {
            if (!strcmp(s.l[i], " 1.11 (VAXB)                      1           EWA-0          0"))
                found_row = 1;
            if (!strcmp(s.l[i], "Executor node = 1.10 (OVMXA)"))
                found_exec = 1;
        }
        NS2_CHECK(found_row && found_exec,
                  "the live table prints NETACP's own values in the oracle columns"
                  " (' 1.11 (VAXB)  ... 1 ... EWA-0  0')");
        memset(&s, 0, sizeof s);
        (void)dnet_netshow_fetch(show_cli_query, &sc, DNET_NETSHOW_ENT_EXECUTOR, &v);
        dnet_netshow_fmt_executor(&v, DNET_NETSHOW_EXEC_COUNTERS, show_collect, &s);
        int hwm = 0;
        for (int i = 0; i < s.n; i++)
            if (!strcmp(s.l[i], "           2  Maximum logical links active")) hwm = 1;
        NS2_CHECK(hwm, "SHOW EXECUTOR COUNTERS prints NETACP's counted high-water (2)");
    }

    for (int i = 0; i < NETACP_MAX_SESSIONS; i++)
        netacp_slot_end(&g_nbslots[i], -1, 0, monotonic_sec(), "selftest teardown");
    g_nb_wire_fd = -1;
    g_netacp_tx = scs_datalink_send;
    g_netacp_reply = netacp_reply_mbx;
    g_netacp_self_set = 0;
    if (sv[0] >= 0) { close(sv[0]); close(sv[1]); }
    unlink(dbpath);

    /* ---------------- C. the formatters reproduce the real VAX ---------------- */
    {
        static struct dnet_netshow_view ov;
        static struct show_lines o, s;
        memset(&ov, 0, sizeof ov);
        ov.as_of = 1791148795u;
        ov.exec.addr = (1u << 10) | 1u; ov.exec.state_on = 1;
        ov.exec.type = DNET_NETSHOW_TYPE_NONROUTING;
        snprintf(ov.exec.name, sizeof ov.exec.name, "VAX1");
        /* The oracle node's own Identification, taken from its transcript (the
         * fixture copies the VAX's values; only the layout is under test). */
        if (show_oracle("MCR_NCP_SHOW_EXECUTOR.txt", &o) == 0)
            for (int i = 0; i < o.n; i++)
                if (!strncmp(o.l[i], "Identification           = ", 27))
                    snprintf(ov.exec.ident, sizeof ov.exec.ident, "%s", o.l[i] + 27);
        snprintf(ov.exec.circuit, sizeof ov.exec.circuit, "QNA-0");
        ov.exec.nsp_ver[0] = 4; ov.exec.nsp_ver[1] = 1; ov.exec.nsp_ver[2] = 0;
        ov.exec.rtg_ver[0] = 2; ov.exec.max_links = 32; ov.exec.max_links_active = 3;
        ov.nnodes = 2;
        ov.nodes[0].addr = (1u << 10) | 2u;  snprintf(ov.nodes[0].name, 7, "VAX2");
        ov.nodes[1].addr = (1u << 10) | 42u; snprintf(ov.nodes[1].name, 7, "OVMX");

        int have = show_oracle("MCR_NCP_SHOW_EXECUTOR.txt", &o) == 0;
        memset(&s, 0, sizeof s);
        dnet_netshow_fmt_executor(&ov, DNET_NETSHOW_EXEC_SUMMARY, show_collect, &s);
        NS2_CHECK(have && show_equal(&s, &o, "SHOW EXECUTOR"),
                  "NCP SHOW EXECUTOR matches the real VAX transcript line for line");

        have = show_oracle("MCR_NCP_SHOW_KNOWN_NODES.txt", &o) == 0;
        memset(&s, 0, sizeof s);
        dnet_netshow_fmt_known_nodes(&ov, show_collect, &s);
        NS2_CHECK(have && show_equal(&s, &o, "SHOW KNOWN NODES"),
                  "NCP SHOW KNOWN NODES matches the real VAX transcript line for line (table columns included)");

        have = show_oracle("MCR_NCP_SHOW_NODE_1.2.txt", &o) == 0;
        memset(&s, 0, sizeof s);
        dnet_netshow_fmt_node(&ov, &ov.nodes[0], show_collect, &s);
        NS2_CHECK(have && show_equal(&s, &o, "SHOW NODE 1.2"),
                  "NCP SHOW NODE 1.2 matches the real VAX transcript line for line");

        have = show_oracle("MCR_NCP_SHOW_KNOWN_LINKS.txt", &o) == 0;
        memset(&s, 0, sizeof s);
        dnet_netshow_fmt_known_links(&ov, show_collect, &s);
        NS2_CHECK(have && show_equal(&s, &o, "SHOW KNOWN LINKS"),
                  "NCP SHOW KNOWN LINKS with no links matches the real VAX ('No information in database')");

        have = show_oracle("MCR_NCP_SHOW_EXECUTOR_CHARACTERISTICS.txt", &o) == 0;
        memset(&s, 0, sizeof s);
        dnet_netshow_fmt_executor(&ov, DNET_NETSHOW_EXEC_CHAR, show_collect, &s);
        NS2_CHECK(have && show_subseq(&s, &o, 5, 2, "SHOW EXECUTOR CHARACTERISTICS"),
                  "NCP SHOW EXECUTOR CHARACTERISTICS: header exact, and every parameter OVMX"
                  " prints is a real VAX line in the real order (the rest honestly omitted)");

        have = show_oracle("MCR_NCP_SHOW_EXECUTOR_COUNTERS.txt", &o) == 0;
        memset(&s, 0, sizeof s);
        dnet_netshow_fmt_executor(&ov, DNET_NETSHOW_EXEC_COUNTERS, show_collect, &s);
        NS2_CHECK(have && show_subseq(&s, &o, 5, 2, "SHOW EXECUTOR COUNTERS"),
                  "NCP SHOW EXECUTOR COUNTERS: header exact, 'Maximum logical links active' in"
                  " the real column (the routing-loss counters NETACP does not keep are omitted)");

        have = show_oracle("SHOW_NETWORK.txt", &o) == 0;
        char line[128];
        dnet_netshow_fmt_network_line(&ov.exec, line, sizeof line);
        NS2_CHECK(have && o.n >= 2 && o.l[0][0] == '\0' && !strcmp(o.l[1], line),
                  "DCL SHOW NETWORK's DECNET product line matches the real VAX column for column");
    }

    printf("DECNETD-I-NETACPSHOW, %d passed, %d failed\n", pass, fail);
    if (fail == 0 && pass > 0) { printf("DECNETD-NETACP-SHOW-SELFTEST: PASS\n"); return 0; }
    printf("DECNETD-NETACP-SHOW-SELFTEST: FAIL\n");
    return 1;
#undef NS2_CHECK
}

/*
 * ================= --net-loopback-accept-test (rd vms-dda, booted) =================
 * The BOOTED proof that a $QIO on _NET: reaches a NETACP and a real FAL server
 * through it: this process runs a NETACP (its broker request mailbox published
 * as DNET$NETACP_REQ, its pool serving inbound objects) on a thread, and on the
 * main thread a client does exactly what DECNETD --copy does on a booted node:
 * $ASSIGN _NET: and COPY 0"SYSTEM MANAGER"::file over $QIO IO$_ACCESS /
 * WRITEVBLK / READVBLK / DEACCESS -- libvms qio_net_op, the executive
 * mailboxes, NETACP's outbound slot, the local loopback, NETACP's inbound FAL
 * dispatch, a FAL.EXE server process running as SYSTEM, and back. The records
 * are byte-verified through RMS. A bad password completes IO$_ACCESS
 * SS$_INVLOGIN. Nothing reaches the wire (loopback). If a NETACP is already
 * serving on this node, the client uses it instead. Needs /dev/vms + the SYSUAF;
 * with no executive every check FAILS honestly (INV-6) -- run by the booted
 * battery only.
 */
static volatile int g_lb_stop;
static uint32_t g_lb_chan;
static void *lb_netacp_thread(void *v)
{
    (void)v;
    while (!g_lb_stop) {
        dnet_tick_t now = monotonic_sec();
        netacp_broker_drain(g_lb_chan, g_nbslots, &g_nbnode, -1, 0, now, &g_nb_lla);
        netacp_loop_drain(g_nbslots, &g_nbnode, -1, 0, now, &g_nb_lla);
        (void)netacp_service_sessions(g_nbslots, -1, 0, now);
        netacp_loop_drain(g_nbslots, &g_nbnode, -1, 0, now, &g_nb_lla);
        struct timespec ts = { 0, 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    for (int i = 0; i < NETACP_MAX_SESSIONS; i++)
        netacp_slot_end(&g_nbslots[i], -1, 0, monotonic_sec(), "test NETACP shutdown");
    return NULL;
}


/*
 * net_req_probe - the request mailbox's protection and NETACP's sender check,
 * seen from an UNPRIVILEGED process (rd vms-c6d1). Runs in a forked child, which
 * the executive registers as a process of its own (kif_bind re-registers a
 * forked task), dropped to UIC [100,100] holding only SETPRV -- the WORLD
 * category of NETACP's S:RWLP,O:RWLP,G:,W:W request mailbox. Every verdict is
 * written to `wfd` for the parent's checks; the child never prints.
 */
struct netreq_probe {
    uint32_t setident;      /* SETIDENT to [100,100]                          */
    uint32_t assign;        /* $ASSIGN DNET$NETACP_REQ: a channel (W access)  */
    uint32_t read_unpriv;   /* IO$M_NOW read with no privilege: SS$_NOPRIV    */
    uint32_t read_readall;  /* the same read with READALL: still SS$_NOPRIV   */
    uint32_t read_bypass;   /* the same read with BYPASS: allowed             */
    uint32_t nonetmbx;      /* own-PID request without NETMBX: reply status    */
    uint32_t forged;        /* request claiming ANOTHER owner_pid: reply status */
    uint32_t honest;        /* own-PID request with NETMBX: reply status        */
};

/* Wait up to `ticks` x 10 ms for the reply with correlation id `corr` on `rep`;
 * 0 = none came. Any OTHER reply seen meanwhile (to a request that must have
 * been dropped) is recorded in *late. */
static uint32_t net_req_probe_wait(uint32_t rep, uint32_t corr, uint32_t *late,
                                   int ticks)
{
    for (int k = 0; k < ticks; k++) {
        uint8_t buf[DNET_BROKER_RSP_MAX + 16];
        uint32_t got = 0;
        uint32_t st = vms_kif_mbx_read(rep, buf, sizeof buf, &got, 1);
        if (st & 1) {
            struct dnet_broker_rsp rsp;
            if (dnet_broker_rsp_decode(buf, got > sizeof buf ? sizeof buf : got, &rsp) ==
                DNET_BROKER_OK) {
                if (rsp.corr_id == corr)
                    return rsp.status ? rsp.status : 1u;
                *late = rsp.corr_id;
            }
            continue;
        }
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}

static uint32_t net_req_probe_send(uint32_t req, uint32_t corr, uint32_t owner,
                                   uint32_t unit)
{
    struct dnet_broker_req r;
    uint8_t rec[DNET_BROKER_REQ_MAX];
    size_t n = 0;
    memset(&r, 0, sizeof r);
    r.corr_id = corr;
    r.owner_pid = owner;
    r.link_handle = 0xDEAD0000u | corr;      /* no such link: an honest NETACP says FILNOTACC */
    r.reply_unit = unit;
    r.op = DNET_BROKER_OP_CLOSE;
    if (dnet_broker_req_encode(&r, rec, sizeof rec, &n) != DNET_BROKER_OK)
        return SS$_BADPARAM;
    return vms_kif_mbx_write(req, rec, (uint32_t)n);
}

static void net_req_probe(uint32_t result_unit)
{
    struct netreq_probe v;
    struct vms_procinfo self;
    char dev[64];
    uint16_t dl = 0;
    uint32_t req = 0, rep = 0, unit = 0, late = 0, n = 0;
    uint8_t buf[DNET_BROKER_REQ_MAX + 16];
    uint64_t prev = 0;

    memset(&v, 0, sizeof v);
    memset(&self, 0, sizeof self);
    /* This process was $CREPRC'd by the test as UIC [100,100] -- the WORLD
     * category of NETACP's request mailbox -- authorized for exactly the
     * privileges toggled below. Start with only TMPMBX enabled. */
    (void)vms_kif_getjpi_self(&self);
    v.setident = (self.uic == ((100u << 16) | 100u)) ? 1u : 0u;
    (void)vms_kif_setprv(PRV$M_NETMBX | PRV$M_READALL | PRV$M_BYPASS, 0, 0, &prev);
    if (vms_kif_lnm_translate(VMS_LNM_TBL_SYSTEM, NETACP_REQ_LOGNAM, 0, dev,
                              sizeof dev - 1, &dl, NULL, NULL) == 1 && dl > 0) {
        dev[dl] = '\0';
        v.assign = vms_kif_mbx_assign(dev, &req);
    }
    v.read_unpriv = vms_kif_mbx_read(req, buf, sizeof buf, &n, 1);
    {
        char rdev[64];
        (void)vms_kif_mbx_create_prot(0, DNET_BROKER_RSP_MAX + 16,
                                      (DNET_BROKER_RSP_MAX + 16) * 8, 0xFF00u,
                                      &rep, &unit, rdev, sizeof rdev);
    }
    /* 1: our own PID, but no NETMBX -> dropped, no reply. NETACP reads the
     * writer's privileges when it serves the request, so NETMBX stays off until
     * this one has had its chance to be (wrongly) answered. */
    (void)net_req_probe_send(req, 1, self.vms_pid, unit);
    v.nonetmbx = net_req_probe_wait(rep, 1, &late, 150);
    (void)vms_kif_setprv(PRV$M_NETMBX, 1, 0, &prev);
    /* 2: NETMBX, but claiming to be ANOTHER process -> dropped; 3: the truth ->
     * served (FILNOTACC: no such link). NETACP serves its request mailbox in
     * order, so once 3 is answered, 2 has been decided: any reply to it would
     * already be here. */
    (void)net_req_probe_send(req, 2, self.vms_pid + 1u, unit);
    (void)net_req_probe_send(req, 3, self.vms_pid, unit);
    v.honest = net_req_probe_wait(rep, 3, &late, 1000);
    v.forged = 0;
    if (late == 1) v.nonetmbx = 0xFFFFFFFFu;     /* a dropped request was answered after all */
    if (late == 2) v.forged = 0xFFFFFFFFu;
    (void)vms_kif_setprv(PRV$M_READALL, 1, 0, &prev);
    v.read_readall = vms_kif_mbx_read(req, buf, sizeof buf, &n, 1);
    (void)vms_kif_setprv(PRV$M_READALL, 0, 0, &prev);
    (void)vms_kif_setprv(PRV$M_BYPASS, 1, 0, &prev);
    v.read_bypass = vms_kif_mbx_read(req, buf, sizeof buf, &n, 1);
    (void)vms_kif_setprv(PRV$M_BYPASS, 0, 0, &prev);
    if (rep) (void)vms_kif_dassgn((uint16_t)rep);
    if (req) (void)vms_kif_dassgn((uint16_t)req);
    {
        char rdev[32];
        uint32_t rc = 0;
        snprintf(rdev, sizeof rdev, "MBA%u:", (unsigned)result_unit);
        if (vms_kif_mbx_assign(rdev, &rc) & 1) {
            (void)vms_kif_mbx_write(rc, &v, sizeof v);
            (void)vms_kif_dassgn((uint16_t)rc);
        }
    }
}

/* The probe process's name carries the unit of the mailbox it reports to: the
 * same rendezvous a FAL.EXE server process uses (dnet_fal_proc.c). */
#define NETPRB_PRCNAM_FMT "NETPRB%u"

/*
 * net_req_probe_spawn - $CREPRC this image as a DETACHED process with UIC
 * [100,100] and only TMPMBX|NETMBX|READALL|BYPASS authorized, the way NETACP
 * creates a FAL server persona -- never a host fork: a VMS process comes from
 * $CREPRC. It finds its role from its process name (main, NETPRB<unit>) and
 * writes its verdicts to MBA<unit>:. Returns 1 with *v filled, 0 otherwise.
 */
static int net_req_probe_spawn(struct netreq_probe *v)
{
    char img[512], staged[512], prcnam[16], dev[32];
    uint32_t ch = 0, unit = 0, pid = 0, n = 0;
    uint64_t privs = PRV$M_TMPMBX | PRV$M_NETMBX | PRV$M_READALL | PRV$M_BYPASS;

    if (vmsfs_to_linux_path("SYS$SYSTEM:DECNETD.EXE", img, sizeof img) != 1)
        return 0;
    if (ovmx_boot_stage_exec_path(img, staged, sizeof staged) && access(staged, X_OK) == 0)
        snprintf(img, sizeof img, "%s", staged);
    if (access(img, X_OK) != 0) {
        /* No runnable copy: say so rather than $CREPRC a process whose image
         * activation then fails after the creation reported success. */
        printf("  NOTE: request-mailbox probe: %s is not executable here (not on the"
               " boot exec stage)\n", img);
        return 0;
    }
    if (!(vms_kif_mbx_create(0, sizeof *v + 16, (sizeof *v + 16) * 2, &ch, &unit,
                             dev, sizeof dev) & 1))
        return 0;
    snprintf(prcnam, sizeof prcnam, NETPRB_PRCNAM_FMT, (unsigned)unit);
    struct dsc$descriptor_s img_d = { (uint16_t)strlen(img), DSC$K_DTYPE_T, DSC$K_CLASS_S, img };
    struct dsc$descriptor_s nam_d = { (uint16_t)strlen(prcnam), DSC$K_DTYPE_T, DSC$K_CLASS_S, prcnam };
    int ok = 0;
    uint32_t st = sys$creprc(&pid, &img_d, NULL, NULL, NULL, &privs, NULL, &nam_d, 0,
                             (100u << 16) | 100u, 0, PRC$M_DETACH);
    if (st & 1) {
        for (int k = 0; k < 2000 && !ok; k++) {       /* up to ~20 s */
            if (vms_kif_mbx_read(ch, v, sizeof *v, &n, 1) & 1)
                ok = (n == sizeof *v);
            else {
                struct timespec ts = { 0, 10 * 1000 * 1000 };
                nanosleep(&ts, NULL);
            }
        }
    }
    if (!ok) {
        struct vms_procinfo pi;
        memset(&pi, 0, sizeof pi);
        uint32_t js = pid ? vms_kif_getjpi_pid(pid, &pi) : 0;
        printf("  NOTE: request-mailbox probe: $CREPRC %s status %08X pid %08X; image %s;"
               " after the wait the process %s\n", prcnam, (unsigned)st, (unsigned)pid, img,
               (js & 1) ? "is still alive" : "is gone");
    }
    (void)vms_kif_dassgn((uint16_t)ch);
    return ok;
}


static int run_net_loopback_accept_test(void)
{
    dnet_tick_t t0 = monotonic_sec();
    printf("DECNETD-I-NETLOOP, $QIO on _NET: brokered through NETACP: COPY 0\"SYSTEM\"::"
           " over the local loopback to a FAL.EXE server process (rd vms-dda)\n");
    int pass = 0, fail = 0;
#define NL_CHECK(c, msg) do { if (c) { pass++; printf("  PASS: %s\n", msg); } \
    else { fail++; printf("  FAIL: %s\n", msg); } } while (0)

    if (!vms_pcb_get())
        vms_pcb_init(0);
    const char *SRC = "SYS$SYSROOT:[SYSMGR]NETLB_SRC.TXT";
    const char *GOT = "SYS$SYSROOT:[SYSMGR]NETLB_GOT.TXT";
    static const char *lines[] = { "NETACP-brokered _NET: COPY over the local loopback" };
    NL_CHECK(rms_textfile_write_line(SRC, lines[0]) == 0,
             "a SYSTEM-owned source file is laid down via RMS");

    pthread_t th;
    int own = 0;
    if (netacp_running() != 1) {
        unsigned ea = 1, en = 1;
        if (sethost_source_executor(&ea, &en) != 0)
            printf("  NOTE: DECnet is not configured here; the test NETACP takes the"
                   " placeholder address 1.1 -- it never touches the wire (loopback only)\n");
        const uint8_t hw[6] = { 0x02,0,0,0,0,0x01 };
        dnet_engine_init(&g_nbnode, ea, en, "OVMXLB", "EWA0", NULL, hw, 0, 0, monotonic_sec());
        memcpy(g_netacp_self, g_nbnode.my_id, 6);
        g_netacp_self_set = 1;
        g_netacp_tx = nb_tx;                        /* counted, never the wire */
        g_nb_wire_fd = -1;
        g_nb_wire = 0;
        memset(g_nbslots, 0, sizeof g_nbslots);
        own = netacp_broker_start(&g_lb_chan);
        NL_CHECK(own, "this process's NETACP published its request mailbox as DNET$NETACP_REQ");
        if (own && pthread_create(&th, NULL, lb_netacp_thread, NULL) != 0)
            own = 0;
    } else {
        printf("  NOTE: a NETACP is already serving on this node -- the client uses it\n");
    }


    struct netcli c;
    int assigned = netcli_assign(&c) == 0;
    NL_CHECK(assigned, "$ASSIGN _NET: granted a channel to the DECnet device face");
    if (assigned) {
        struct dnet_copy_plan plan;
        char spec[160];
        snprintf(spec, sizeof spec, "0\"SYSTEM\"::%s", SRC);
        int rp = dnet_copy_plan(spec, GOT, &plan);
        printf("  NOTE: t+%lus: COPY with the right password\n", (unsigned long)(monotonic_sec() - t0));
        fflush(stdout);
        uint32_t st = (rp == 0) ? copy_client_run_net(&c, &plan, "MANAGER") : SS$_BADPARAM;
        NL_CHECK(st == SS$_NORMAL && fal_file_matches(GOT, lines, 1),
                 "COPY 0\"SYSTEM MANAGER\"::file over $QIO _NET: completed through NETACP, and the"
                 " records BYTE-MATCH (qio_net_op -> mailboxes -> NETACP -> loopback -> FAL.EXE)");
        printf("  NOTE: t+%lus: COPY with a bad password\n", (unsigned long)(monotonic_sec() - t0));
        fflush(stdout);
        st = (rp == 0) ? copy_client_run_net(&c, &plan, "WRONGPW") : SS$_BADPARAM;
        printf("  NOTE: t+%lus: bad-password COPY returned %08X\n",
               (unsigned long)(monotonic_sec() - t0), (unsigned)st);
        NL_CHECK(st == SS$_INVLOGIN,
                 "the same COPY with a bad password completes IO$_ACCESS SS$_INVLOGIN -- no link, no file");
        uint8_t b[8]; size_t x = 0;
        st = netcli_op(&c, DNET_BROKER_OP_RECV, NULL, 0, b, sizeof b, &x);
        NL_CHECK(st == SS$_FILNOTACC, "IO$_READVBLK on the channel after DEACCESS is SS$_FILNOTACC");
        NL_CHECK(sys$dassgn(c.chan) == SS$_NORMAL, "$DASSGN releases the _NET: channel");
    }
    /* The request mailbox, from an unprivileged process (rd vms-c6d1). */
    if (netacp_running() == 1) {
        struct netreq_probe v;
        memset(&v, 0, sizeof v);
        int got = net_req_probe_spawn(&v);
        NL_CHECK(got && (v.setident & 1),
                 "an unprivileged probe process ($CREPRC'd as [100,100]) reported");
        NL_CHECK(v.assign == SS$_NORMAL,
                 "the unprivileged process may $ASSIGN DNET$NETACP_REQ (W:W -- it can submit requests)");
        NL_CHECK(v.read_unpriv == SS$_NOPRIV,
                 "the unprivileged process may NOT read DNET$NETACP_REQ (SS$_NOPRIV) -- no other"
                 " client's request, NCB password included, is readable");
        NL_CHECK(v.read_readall == SS$_NOPRIV,
                 "READALL does not open it either (as on real VMS: oracle mbxprot MBXP.READALL.READ)");
        NL_CHECK(v.read_bypass != SS$_NOPRIV && v.read_bypass != 0,
                 "with BYPASS the same read is permitted -- the VMS privilege override, not a"
                 " special case");
        NL_CHECK(v.nonetmbx == 0, "a request from a process without NETMBX is dropped unanswered");
        NL_CHECK(v.forged == 0,
                 "a request whose owner_pid is not its writer (the executive-stamped sender PID) is"
                 " dropped unanswered");
        NL_CHECK(v.honest == SS$_FILNOTACC,
                 "the same request, truthful and with NETMBX, is served (FILNOTACC: no such link)");
    }

    if (own) {
        g_lb_stop = 1;
        pthread_join(th, NULL);
        NL_CHECK(g_nb_wire == 0, "nothing was put on the wire -- the link ran over the local loopback");
        netacp_broker_stop(g_lb_chan);
    }

    printf("DECNETD-I-NETLOOP, %d passed, %d failed\n", pass, fail);
    if (fail == 0 && pass > 0) { printf("DECNETD-NET-LOOPBACK-ACCEPT: PASS\n"); return 0; }
    printf("DECNETD-NET-LOOPBACK-ACCEPT: FAIL\n");
    return 1;
#undef NL_CHECK
}

int main(int argc, char **argv)
{
    /* rd vms-c6d1: started by net_req_probe_spawn ($CREPRC, no argv -- VMS
     * semantics), this process is the request-mailbox probe if its process name
     * says so. */
    if (argc == 1) {
        struct vms_procinfo me;
        unsigned u = 0;
        memset(&me, 0, sizeof me);
        if ((vms_kif_getjpi_self(&me) & 1) &&
            sscanf(me.prcnam, NETPRB_PRCNAM_FMT, &u) == 1 && u != 0) {
            net_req_probe(u);
            return 0;
        }
    }
    const char *ifname = DECNETD_DEFAULT_IFACE;
    int ifname_explicit = 0;      /* did the caller pin --iface?             */
    const char *addr_s = NULL;
    const char *name = "OVMX";
    int name_explicit = 0;        /* did the caller pin --name?              */
    const char *device = "EWA0";
    const char *circuit = NULL;
    int hello_interval = (int)DNET_T3_DEFAULT;
    int duration = 0;
    int show_executor_only = 0;
    int self_test = 0;
    int nsp_self_test = 0;
    int task_self_test = 0;               /* --task-selftest : task-to-task client floor */
    int net_broker_test = 0;              /* --net-broker-selftest : T1 broker record codec */
    int net_ncb_test = 0;                 /* --net-ncb-selftest : NCB connect-block parser */
    int net_service_test = 0;             /* --net-service-selftest : broker service dispatch */
    int net_mbx_test = 0;                 /* --net-mbx-selftest : T1 mailbox round-trip (/dev/vms) */
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
    int fal_proc_accept_test = 0;
    int copy_self_test = 0;               /* --copy-selftest : COPY command-layer floor */
    int copy_accept_test = 0;             /* --copy-accept-test : full COPY transfer proof */
    int copy_xport_test = 0;              /* --copy-transport-selftest : COPY client pump floor */
    const char *copy_src = NULL;          /* --copy <src> <dst> : outbound FAL COPY      */
    const char *copy_dst = NULL;
    int copy_password_fd = -1;            /* --password-fd N : the FAL password source   */
    int router_mode = 0;           /* --router: emit router-hellos, advertise L1 router */
    int router_priority = 0;       /* --priority: DR-election priority (0 => DNA default 64) */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--address") && i + 1 < argc)      addr_s = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc)    { name = argv[++i]; name_explicit = 1; }
        else if (!strcmp(argv[i], "--iface") && i + 1 < argc)   { ifname = argv[++i]; ifname_explicit = 1; }
        else if (!strcmp(argv[i], "--device") && i + 1 < argc)  device = argv[++i];
        else if (!strcmp(argv[i], "--circuit") && i + 1 < argc) circuit = argv[++i];
        else if (!strcmp(argv[i], "--hello-interval") && i + 1 < argc)
            hello_interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--duration") && i + 1 < argc)
            duration = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--show-executor")) show_executor_only = 1;
        else if (!strcmp(argv[i], "--self-test"))     self_test = 1;
        else if (!strcmp(argv[i], "--nsp-selftest"))  nsp_self_test = 1;
        else if (!strcmp(argv[i], "--task-selftest")) task_self_test = 1;
        else if (!strcmp(argv[i], "--net-broker-selftest")) net_broker_test = 1;
        else if (!strcmp(argv[i], "--net-ncb-selftest")) net_ncb_test = 1;
        else if (!strcmp(argv[i], "--net-service-selftest")) net_service_test = 1;
        else if (!strcmp(argv[i], "--net-mbx-selftest")) net_mbx_test = 1;
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
        else if (!strcmp(argv[i], "--fal-proc-accept-test")) fal_proc_accept_test = 1;
        else if (!strcmp(argv[i], "--netacp-pool-selftest")) return run_netacp_pool_selftest();
        else if (!strcmp(argv[i], "--mail11-accept-test")) return run_mail11_accept_test();
        else if (!strcmp(argv[i], "--netacp-broker-selftest")) return run_netacp_broker_selftest();
        else if (!strcmp(argv[i], "--netacp-show-selftest")) {
            if (i + 2 < argc && !strcmp(argv[i + 1], "--oracle-dir"))
                g_show_oracle_dir = argv[i + 2];
            return run_netacp_show_selftest();
        }
        else if (!strcmp(argv[i], "--net-loopback-accept-test")) return run_net_loopback_accept_test();
        else if (!strcmp(argv[i], "--copy-selftest")) copy_self_test = 1;
        else if (!strcmp(argv[i], "--copy-accept-test")) copy_accept_test = 1;
        else if (!strcmp(argv[i], "--copy-transport-selftest")) copy_xport_test = 1;
        else if (!strcmp(argv[i], "--copy") && i + 2 < argc) {
            copy_src = argv[++i];
            copy_dst = argv[++i];
        }
        else if (!strcmp(argv[i], "--password-fd") && i + 1 < argc)
            copy_password_fd = atoi(argv[++i]);
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
    if (task_self_test)
        return run_task_selftest();
    if (net_broker_test)
        return run_net_broker_selftest();
    if (net_ncb_test)
        return run_net_ncb_selftest();
    if (net_service_test)
        return run_net_service_selftest();
    if (net_mbx_test)
        return run_net_mbx_selftest();
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
    if (fal_proc_accept_test)
        return run_fal_proc_accept_test();
    if (copy_self_test)
        return run_copy_selftest();
    if (copy_xport_test)
        return run_copy_transport_selftest();
    if (copy_accept_test)
        return run_copy_accept_test();
    /* --copy (run_copy_loop) needs the live datalink; dispatched after the socket
     * is opened, beside the --set-host client (below). */

    /* A router routes and a --set-host CLIENT bridges a terminal; neither is a
     * NETACP that serves inbound object-42 sessions unless the caller pins it on.
     * The persistent ENDNODE daemon serves by default (see cterm_server above). */
    if ((router_mode || set_host_to) && !cterm_server_explicit)
        cterm_server = 0;

    /* OUTBOUND CLIENTS GO THROUGH NETACP (rd vms-dda). On VMS every logical link
     * belongs to NETACP; a COPY / SET HOST image reaches one through $ASSIGN
     * _NET: + $QIO, never by running a second NSP engine on the node's address
     * (which would race NETACP for every inbound segment, and which the
     * executive refuses the datalink anyway). So when a NETACP is serving, the
     * client is brokered through it. With NO NETACP: on the booted runtime (the
     * executive datalink backend) that is an honest failure -- DECnet is not
     * started -- never a private engine; only the host/lab AF_PACKET instrument
     * build still runs its own engine (it has no NETACP to defer to). */
    if (copy_src || set_host_to) {
        int nr = netacp_running();
        if (nr == 1 && !ifname_explicit) {
            if (copy_src)
                return run_copy_net(copy_src, copy_dst, copy_password_fd);
            struct netcli c;
            if (netcli_assign(&c) != 0)
                return 1;
            struct dnet_executor ex;
            const char *local = "0";
            static char lname[DNET_NODEDB_NAMEMAX + 8];
            if (dnet_store_load_executor(&ex) == DNET_STORE_OK) {
                if (ex.name[0])
                    snprintf(lname, sizeof lname, "%s", ex.name);
                else if (ex.have_addr)
                    snprintf(lname, sizeof lname, "%u.%u", dnet_area_of(ex.addr),
                             dnet_node_of(ex.addr));
                if (lname[0]) local = lname;
            }
            int r = run_set_host_net(&c, set_host_to, local);
            sys$dassgn(c.chan);
            return r;
        }
        if (strcmp(scs_datalink_backend(), "executive") == 0 && !ifname_explicit) {
            fprintf(stderr, "DECNETD-E-NONETACP, no NETACP is serving logical links"
                            " on this node (SS$_DEVOFFLINE): outbound DECnet links are"
                            " brokered through NETACP -- start DECnet with"
                            " @SYS$MANAGER:STARTNET\n");
            return 1;
        }
    }

    /* RESOLVE THE DATALINK INTERFACE. When --iface was not given (the persistent
     * NETACP daemon STARTNET.COM starts with no argv), auto-detect the primary
     * NIC rather than binding the compile-time "br0" -- inside a booted node's
     * netns the NIC is eth0/ETH0:, not the dev-lab bridge (rd vms-a70 direction
     * B, gap B). Explicit --iface (the veth/lab harness) always wins; if
     * detection finds nothing the compiled default stands and the open below
     * fails honestly. */
    static char ifname_auto[IF_NAMESIZE];
    int ifname_detected = 0;     /* did auto-detection actually pick a NIC?   */
    if (!ifname_explicit && decnet_autodetect_iface(ifname_auto, sizeof(ifname_auto))) {
        ifname = ifname_auto;
        ifname_detected = 1;
    }

    /* SELF-SOURCE the executor address from the node's DECnet configuration
     * (executor.dat, rd vms-f54) whenever --address was not given -- for the
     * --set-host CLIENT (so DCL's SET HOST wiring need not know it), for the
     * persistent NETACP daemon SYS$MANAGER:STARTNET.COM starts with no argv
     * (rd vms-a70 direction B), and for --show-executor. If executor.dat is
     * absent the NOADDRESS error below fires -- DECnet is simply not configured
     * on this node, and no address is ever invented (INV-6). */
    static char sethost_addrbuf[16];
    static char exec_namebuf[DNET_NODEDB_NAMEMAX + 1];
    if (!addr_s) {
        unsigned ea = 0, en = 0;
        if (sethost_source_executor(&ea, &en) == 0) {
            snprintf(sethost_addrbuf, sizeof(sethost_addrbuf), "%u.%u", ea, en);
            addr_s = sethost_addrbuf;
            /* The executor NAME comes from the same database (rd vms-30e): a
             * self-sourced NETACP is the node NCP configured, so NCP SHOW
             * EXECUTOR reads back NCP's name -- never the compiled-in default.
             * An unnamed executor stays unnamed. */
            struct dnet_executor ex;
            if (!name_explicit && dnet_store_load_executor(&ex) == DNET_STORE_OK) {
                snprintf(exec_namebuf, sizeof exec_namebuf, "%s", ex.name);
                name = exec_namebuf;
            }
        }
    }

    /* Identity is required and never invented (INV-6; the scsd
     * resolve_node_identity discipline: a wrong identity must never be made up). */
    unsigned area = 0, node = 0;
    if (!addr_s || parse_addr(addr_s, &area, &node) != 0) {
        /* BARE AUTO-START on an UNCONFIGURED node (argc == 1: the persistent
         * NETACP daemon SYS$MANAGER:STARTNET.COM launches with no argv, having
         * found no executor address in the node's DECnet configuration). This is
         * NOT an error -- an unconfigured node simply runs no DECnet. Exit CLEAN
         * (success), logging the honest no-op, so STARTNET's RUN/DETACHED leaves
         * neither a failed process nor a %DCL abort on the boot console (INV-6).
         * An EXPLICIT invocation (any flag: --set-host, --show-executor, --router,
         * ...) with no resolvable address is still the caller's error below. */
        if (argc == 1) {
            printf("DECNETD-I-NOCONFIG, DECnet is not configured on this node"
                   " (no executor address); NETACP not started\n");
            fflush(stdout);
            return 0;
        }
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
        /* The Linux datalink the daemon WOULD bind (auto-detected primary NIC
         * unless --iface pinned it) -- the dry-run readout of gap-B resolution;
         * no socket is opened here. */
        printf("Datalink interface = %s%s\n", ifname,
               ifname_explicit ? " (--iface)"
               : ifname_detected ? " (auto-detected primary NIC)"
               : " (compiled default; no usable NIC detected)");
        /* Which raw-L2 path this binary was BUILT with (rd vms-1f69) -- a
         * compile-time fact, no runtime fallback between them: "executive"
         * (/dev/vms VMS_IOCTL_L2_*, PHY_IO; the booted runtime), "AF_PACKET
         * probe" (CAP_NET_RAW; a host/lab instrument) or "bpf" (NetBSD). */
        printf("Datalink backend = %s\n", scs_datalink_backend());
        printf("Executor database = %s\n",
               dnet_store_location(DNET_STORE_EXECUTOR));
        return 0;
    }

    /* Open the raw-L2 datalink (INV-6 fail-honest: no per-process fake if the
     * netdev cannot be opened). Same abstraction scsd.c used.
     *
     * STATION ADDRESS (rd vms-1f69). A Phase IV node sources every frame from
     * its ALGORITHMIC address AA-00-04-00-<LE16(area<<10|node)> (the engine
     * already writes that as each frame's source -- dnet_engine.c my_id) and
     * receives unicast addressed to it. On the booted runtime the datalink is
     * the EXECUTIVE's (VMS_IOCTL_L2_OPEN, gated on PHY_IO, no CAP_NET_RAW):
     * it validates this station and stamps it on every send. */
    uint8_t station[6];
    {
        unsigned a16 = ((area & 0x3fu) << 10) | (node & 0x3ffu);
        station[0] = 0xAA; station[1] = 0x00; station[2] = 0x04; station[3] = 0x00;
        station[4] = (uint8_t)(a16 & 0xffu);
        station[5] = (uint8_t)((a16 >> 8) & 0xffu);
    }
    int sock = scs_datalink_open_station(ifname, DNET_ETHERTYPE, station);
    if (sock < 0) {
        int e = errno;
        if (strcmp(scs_datalink_backend(), "executive") == 0) {
            uint32_t vst = scs_datalink_last_status();
            const char *why =
                (vst == SS$_NOPRIV)    ? "SS$_NOPRIV -- the executive refused:"
                                         " NETACP requires the PHY_IO privilege"
              : (vst == SS$_NOSUCHDEV) ? "SS$_NOSUCHDEV -- no such network interface"
              : (vst == SS$_BADPARAM)  ? "SS$_BADPARAM -- the executive refused the"
                                         " station address"
              : (vst == SS$_ABORT)     ? "SS$_ABORT -- the executive could not open"
                                         " the interface's raw socket"
              : vst                    ? "the executive refused the datalink"
                                       : strerror(e);
            fprintf(stderr,
                    "DECNETD-E-NOSOCKET, executive datalink open on '%s'"
                    " (ethertype 0x%04x) failed: %s (status %%X%08X)\n",
                    ifname, (unsigned)DNET_ETHERTYPE, why, (unsigned)vst);
            if (scs_datalink_last_stv())
                fprintf(stderr, "-DECNETD-I-HOSTERR, the executive's datalink"
                        " backend returned host errno %u\n",
                        (unsigned)scs_datalink_last_stv());
            /* Say WHOSE privileges the executive judged: this process's own
             * executive row (pid, user, current privilege mask, PHY_IO bit),
             * read back from the executive -- so a refusal is diagnosable
             * from the log alone (rd vms-1f69). */
            struct vms_procinfo me;
            memset(&me, 0, sizeof(me));
            if (vms_kif_getjpi_self(&me) & 1)
                fprintf(stderr,
                        "-DECNETD-I-PROCPRIV, executive row pid %08X user %.12s"
                        " curpriv %016llX (PHY_IO %s)\n",
                        (unsigned)me.vms_pid, me.username,
                        (unsigned long long)me.cur_privs,
                        (me.cur_privs & (1ULL << 22)) ? "held" : "NOT held");
        } else {
            fprintf(stderr,
                    "DECNETD-E-NOSOCKET, %s datalink open on '%s' (0x%04x)"
                    " failed: %s\n"
                    "  (the AF_PACKET probe needs CAP_NET_RAW -- a host/lab"
                    " instrument; the booted runtime builds the executive"
                    " backend, OVMX_DATALINK_VIA_EXECUTIVE)\n",
                    scs_datalink_backend(), ifname, (unsigned)DNET_ETHERTYPE,
                    strerror(e));
        }
        return 1;
    }
    log_ts(stdout);
    printf(" DECNETD-I-DATALINK, datalink open on %s via the %s"
           " (%s), station address %02X-%02X-%02X-%02X-%02X-%02X\n",
           ifname, scs_datalink_backend(),
           strcmp(scs_datalink_backend(), "executive") == 0
               ? "VMS_IOCTL_L2_OPEN, PHY_IO; no CAP_NET_RAW"
               : "not executive-resident",
           station[0], station[1], station[2], station[3], station[4], station[5]);
    fflush(stdout);
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
           " (wire engine demoted to NETACP's datalink; the raw L2 path hidden"
           " behind the _NET: device face, Rule 1)\n",
           router_mode ? "L1 router" : "endnode", eng.circuit);
    dnet_engine_show_executor(&eng, stdout);
    dnet_engine_show_circuit(&eng, stdout);
    /* Say, honestly, whether this NETACP serves inbound $ SET HOST. When it
     * does, an inbound object-42 connect reaches LOGINOUT on an executive-minted
     * RTAn: (a bounded pool of sessions, rd vms-6af1); the remote user authenticates fresh. */
    if (cterm_server) {
        /* The pool is the executor's MAXIMUM LINKS (rd vms-f91). An
         * unreadable executor database leaves the VMS default. */
        struct dnet_executor ex;
        int clamped = 0;
        if (dnet_store_load_executor(&ex) != DNET_STORE_OK) memset(&ex, 0, sizeof ex);
        g_netacp_max_links = netacp_pool_size(&ex, &clamped);
        if (clamped) {
            log_ts(stdout);
            printf(" DECNETD-W-MAXLINKS, executor MAXIMUM LINKS %u exceeds this NETACP's"
                   " %d session slots; serving %d\n", dnet_executor_max_links(&ex),
                   NETACP_POOL_CAP, NETACP_POOL_CAP);
        }
    }
    log_ts(stdout);
    if (cterm_server)
        printf(" DECNETD-I-CTERMLISTEN, serving inbound $ SET HOST (Session"
               " Control object 42 -> LOGINOUT on RTAn:) and file access (object"
               " 17 -> FAL.EXE as the user); up to %d sessions, %d per node\n",
               NETACP_MAX_SESSIONS, NETACP_MAX_PER_SOURCE);
    else
        printf(" DECNETD-I-ROUTEONLY, NOT serving inbound $ SET HOST"
               " (routing only)\n");
    fflush(stdout);

    /* --set-host CLIENT (rd vms-f54): the OUTBOUND half of $ SET HOST. It opens
     * a CTERM terminal session to object 42 on the remote node and bridges THIS
     * process's VMS terminal channel to it, then returns -- it owns its own loop
     * and never falls through to the routing/inbound loop below. */
    /* --copy CLIENT (rd vms-ea8): the OUTBOUND $ COPY over the live datalink. It
     * opens an object-17 FAL logical link to the remote node, drives the DAP
     * transfer, and returns -- it owns its own transfer and never falls through
     * to the routing/inbound loop below. */
    if (copy_src) {
        int r = run_copy_loop(&eng, sock, ifindex, copy_src, copy_dst,
                              copy_password_fd);
        scs_datalink_close(sock);
        return r;
    }

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

    /* The logical links this node holds -- inbound sessions AND (rd vms-dda)
     * outbound links local processes open through _NET: -- a bounded pool of
     * the executor's MAXIMUM LINKS (rd vms-f91). */
    static struct netacp_slot slots[NETACP_POOL_CAP];
    memset(slots, 0, sizeof slots);
    uint16_t next_lla = 0x2100;
    memcpy(g_netacp_self, eng.my_id, 6);
    g_netacp_self_set = 1;
    g_netacp_serve_inbound = cterm_server;
    uint32_t breq_chan = 0;
    int broker = router_mode ? 0 : netacp_broker_start(&breq_chan);

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

        /* Serve every live session's local side (never blocks), then wait on
         * the wire only briefly while sessions are live -- so a session's
         * output is not held back by an idle wire. */
        int live = 0;
        if (cterm_server || broker)
            live = netacp_service_sessions(slots, sock, ifindex, now);
        if (broker)
            netacp_broker_drain(breq_chan, slots, &eng, sock, ifindex, now, &next_lla);
        netacp_loop_drain(slots, &eng, sock, ifindex, now, &next_lla);
        int outbound = 0;
        for (int i = 0; i < NETACP_MAX_SESSIONS; i++)
            outbound += slots[i].used && slots[i].outbound;
        /* A client polls its outbound link through the broker, so while one is
         * live the wire wait is short; idle, it bounds request latency. */
        (void)scs_datalink_set_recv_timeout_ms(sock, outbound ? 5 : live ? 20
                                                     : broker ? 100 : 250);

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

        /* INBOUND SESSION DISPATCH (rd vms-f40 CTERM 42, rd vms-d85 FAL 17,
         * rd vms-6af1 pool). A frame the routing/HELLO path did not claim may be
         * an NSP logical-link frame: it is demultiplexed to its session by our
         * logical-link address, or -- a Connect Initiate -- offered to a free
         * slot. UNTRUSTED, UNAUTHENTICATED bytes, parsed under bounds; every
         * refusal leaves the peer disconnected rather than admitted (INV-6). */
        if ((cterm_server || broker) && rc != 1)
            netacp_dispatch_frame(slots, &eng, sock, ifindex, rxbuf, (size_t)n,
                                  now, &next_lla);
        netacp_loop_drain(slots, &eng, sock, ifindex, now, &next_lla);

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

    for (int i = 0; i < NETACP_MAX_SESSIONS; i++)
        netacp_slot_end(&slots[i], sock, ifindex, monotonic_sec(), "NETACP shutdown");
    if (broker)
        netacp_broker_stop(breq_chan);

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
