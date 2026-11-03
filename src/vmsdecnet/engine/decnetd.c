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
#include <ifaddrs.h>    /* probe build: host NIC auto-detect (the executive build names ETH0:) */
#include <poll.h>       /* the --fal-proc-accept-test pump only (selftest; rd vms-d3cd) */
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
#include "opcdef.h"         /* rd vms-158d: the "DECnet starting" OPCOM message */
#include "iodef.h"          /* IO$_READVBLK/WRITEVBLK/SETMODE + IO$K_TT_PASSALL      */
#include "lnmdef.h"         /* LNM$C_USER: the DNET$NETACP_REQ logical (rd vms-dda)   */

/* The terminal channel's backing fd. Since rd vms-d3cd nothing waits on it:
 * readiness is the terminal driver's type-ahead count ($QIO IO$_SENSEMODE!
 * IO$M_TYPEAHDCNT). It is still consulted once, to learn whether SYS$INPUT is
 * a terminal at all (sethost_open_terminal). */
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

/*
 * THE LAN UNIT, THE VMS WAY (rd vms-d3cd). The booted NETACP (the executive
 * datalink build) does no host interface enumeration -- no socket(AF_INET) +
 * SIOCGIF*, no getifaddrs, no if_nametoindex. It names the executive's LAN
 * controller unit ETH0: (the EWA0:-style DC$_SCOM row the executive entered
 * for the primary NIC) and asks it, as a VMS program asks a LAN device:
 * $ASSIGN ETH0:, $QIOW IO$_SENSEMODE, and the hardware address is the
 * NMA$C_PCLI_HWA entry of the extended characteristics (sys_qio.c
 * qio_lan_op). The datalink itself is opened on that VMS name too, and the
 * executive resolves the host interface privately (VMS_IOCTL_L2_OPEN).
 */
#define DECNETD_LAN_UNIT "ETH0:"

static int decnetd_executive_datalink(void)
{
    return strcmp(scs_datalink_backend(), "executive") == 0;
}

/* Is `n` a VMS device name (carries a ':'), not a host interface name? */
static int decnetd_is_vms_devnam(const char *n)
{
    return n && strchr(n, ':') != NULL;
}

/* $QIOW IO$_SENSEMODE on ETH0: -> its hardware address. Returns the VMS
 * status: the $ASSIGN's, the $QIOW's, the IOSB's, or SS$_NOSUCHDEV when the
 * extended characteristics carry no hardware address. */
static uint32_t decnetd_lan_sense_hwaddr(uint8_t mac_out[6])
{
    if (!vms_pcb_get())
        vms_pcb_init(0);
    struct dsc$descriptor_s dev;
    dev.dsc$w_length = (uint16_t)strlen(DECNETD_LAN_UNIT);
    dev.dsc$b_dtype = DSC$K_DTYPE_T;
    dev.dsc$b_class = DSC$K_CLASS_S;
    dev.dsc$a_pointer = (char *)DECNETD_LAN_UNIT;
    uint16_t ch = 0;
    uint32_t st = sys$assign(&dev, &ch, 0, NULL);
    if (!(st & 1))
        return st;
    uint8_t chars[8];
    uint8_t ext[64];
    struct dsc$descriptor_s xd;
    xd.dsc$w_length = (uint16_t)sizeof ext;
    xd.dsc$b_dtype = DSC$K_DTYPE_T;
    xd.dsc$b_class = DSC$K_CLASS_S;
    xd.dsc$a_pointer = (char *)ext;
    struct _iosb iosb;
    memset(&iosb, 0, sizeof iosb);
    memset(ext, 0, sizeof ext);
    /* P2 is the extended-characteristics descriptor; OVMX takes it in P4 when
     * a longword P2 cannot hold its address (sys_qio.c qio_lan_op, Rule 8). */
    st = sys$qiow(0, ch, IO$_SENSEMODE, &iosb, NULL, 0, chars, sizeof chars,
                  0, (uintptr_t)&xd, 0, 0);
    if (st & 1)
        st = iosb.iosb$w_status;
    (void)sys$dassgn(ch);
    if (!(st & 1))
        return st;
    /* Walk the parameter entries for NMA$C_PCLI_HWA (1160, string). */
    size_t n = iosb.iosb$w_bcnt <= sizeof ext ? iosb.iosb$w_bcnt : sizeof ext;
    size_t i = 0;
    while (i + 2 <= n) {
        uint16_t id = (uint16_t)(ext[i] | (ext[i + 1] << 8));
        i += 2;
        if (id & 0x1000u) {                    /* NMA$M_PTY_STR: word count + bytes */
            if (i + 2 > n) break;
            uint16_t len = (uint16_t)(ext[i] | (ext[i + 1] << 8));
            i += 2;
            if (i + len > n) break;
            if ((id & 0x0FFFu) == 1160u && len == 6) {
                memcpy(mac_out, ext + i, 6);
                return SS$_NORMAL;
            }
            i += len;
        } else {
            i += 4;                            /* a longword value */
        }
    }
    return SS$_NOSUCHDEV;
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

/*
 * Entropy for this start's NSP logical-link addresses (rd vms-3106). It mixes
 * the kernel RNG with the WALL clock and the pid. The monotonic clock is not
 * used because it restarts at boot. So a node that reboots does not begin
 * again at the address of a link a peer may still hold.
 */
static uint64_t dnet_lla_entropy(void)
{
    uint64_t e = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, &e, sizeof e) != (ssize_t)sizeof e) e = 0;
        close(fd);
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    e ^= ((uint64_t)ts.tv_sec << 20) ^ (uint64_t)ts.tv_nsec ^ ((uint64_t)getpid() << 40);
    return e;
}

/* The address a one-link client (SET HOST, COPY run straight over the datalink)
 * uses for its link: a fresh per-start value, never a fixed constant. */
static uint16_t dnet_client_lla(void)
{
    uint16_t cur = dnet_lla_seed(dnet_lla_entropy());
    return dnet_lla_take(&cur, NULL, NULL);
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

/*
 * NETACP's VOLATILE node database (rd vms-acf3 follow-up). As on VMS, the
 * running network works from its volatile copy: it is loaded from the
 * permanent database (SYS$SYSTEM:NETNODE_REMOTE.DAT, what NCP DEFINE writes)
 * when NETACP starts serving, every NETACP lookup reads it -- connect
 * resolution, NCP SHOW, the remote port info and peer names of inbound
 * sessions -- and NCP SET/CLEAR NODE change it through NETACP
 * (netacp_netctl), never the file. NCP DEFINE/PURGE NODE change the file
 * only; the next NETACP start loads it. So a SET HOST no longer reads a file
 * at all: it used to re-read NETNODE_REMOTE.DAT on every lookup, which was
 * both unlike VMS and what a cross-node file-lock collision (rd vms-326)
 * turned into SET HOST failures.
 *
 * Before NETACP serves (self-tests, the --set-host client, COPY), the
 * permanent file is what a lookup reads, as before.
 */
static struct dnet_nodedb g_vnodes;
static int g_vnodes_live;

/* Load the volatile database from the permanent one: NETACP start. A missing
 * permanent database is an empty volatile one; an unreadable one is reported
 * and leaves it empty (the network still starts; SET NODE can fill it). */
static void netacp_vnodes_load(void)
{
    int rc = dnet_store_load_nodes(&g_vnodes);
    if (rc == DNET_STORE_EREAD) {
        log_ts(stderr);
        fprintf(stderr, " DECNETD-E-NODEDB, cannot read the permanent node database"
                        " (status %08X); the volatile database starts empty\n",
                (unsigned)dnet_store_last_rms_status());
        fflush(stderr);
        dnet_nodedb_init(&g_vnodes);
    } else if (rc != DNET_STORE_OK) {
        dnet_nodedb_init(&g_vnodes);
    }
    g_vnodes_live = 1;
}

/* The node database a lookup reads: NETACP's volatile copy while it serves,
 * else the permanent file. NULL (and *err = DNET_STORE_EREAD) when the file
 * could not be read. */
static const struct dnet_nodedb *netacp_nodedb(int *err)
{
    static struct dnet_nodedb file_db;
    if (err) *err = DNET_STORE_OK;
    if (g_vnodes_live)
        return &g_vnodes;
    int rc = dnet_store_load_nodes(&file_db);
    if (rc != DNET_STORE_OK) {
        if (err) *err = rc;
        return NULL;
    }
    return &file_db;
}

/* Resolve a --set-host target: accept "area.node" directly, else look the token
 * up as a NODE NAME (case-insensitive) in the remote node database. Returns 0
 * and fills area/node; -1 when the database does not list the name; -2 when
 * the database could not be READ (its RMS status is
 * dnet_store_last_rms_status()) -- a node NETACP cannot look up is not a node
 * that is unknown (rd vms-acf3). */
static int sethost_resolve_target(const char *token, unsigned *area, unsigned *node)
{
    if (parse_addr(token, area, node) == 0)
        return 0;
    int lrc = DNET_STORE_OK;
    const struct dnet_nodedb *db = netacp_nodedb(&lrc);
    if (lrc == DNET_STORE_EREAD)
        return -2;
    if (!db)
        return -1;
    const struct dnet_node_entry *e = dnet_nodedb_by_name(db, token);
    if (!e)
        return -1;
    *area = dnet_area_of(e->addr);
    *node = dnet_node_of(e->addr);
    return 0;
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
    int rrc = 0;
    if (dnet_ncb_parse((const char *)ncbtxt, ncblen, &n) != DNET_NCB_OK)
        return SS$_BADPARAM;

    if (strcmp(n.node, "0") == 0 || strcasecmp(n.node, node->node_name) == 0) {
        *rarea = dnet_area_of(node->addr);
        *rnode = dnet_node_of(node->addr);
    } else if ((rrc = sethost_resolve_target(n.node, rarea, rnode)) != 0) {
        /* Not area.node and not in the node database: VMS's condition for that
         * is SS$_NOSUCHNODE, "remote node is unknown" (SSDEF 652, oracle-pinned
         * in ssdef.h). It was SS$_NOSUCHDEV, so a visitor's SET HOST VAXC said
         * "%SYSTEM-W-NOSUCHDEV, no such device" (rd vms-5843).
         * A node database NETACP could not READ is not "unknown": the request
         * fails with the RMS status of that read (rd vms-acf3). */
        if (rrc == -2) {
            uint32_t rst = dnet_store_last_rms_status();
            if (rst == 0 || (rst & 1))
                rst = RMS$_RER;
            fprintf(stderr, "DECNETD-E-NODEDB, cannot read the node database to resolve '%s' (status %08X)\n",
                    n.node, (unsigned)rst);
            fflush(stderr);
            /* A NET: completion carries an SS$ (word) status: the RMS
             * failure's SS$ form, e.g. RMS$_FLK -> SS$_ACCONFLICT. */
            st = dnet_broker_iosb_status(rst);
        } else {
            st = SS$_NOSUCHNODE;
        }
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
 * its VMS terminal CHANNELS -- $ASSIGN SYS$INPUT / SYS$OUTPUT, and TT: (the
 * job's terminal unit) when SYS$INPUT is that terminal -- and $QIO
 * IO$_READVBLK / IO$_WRITEVBLK to move bytes: the SAME executive terminal path
 * a console login or an RTAn: device uses. It NEVER calls tcsetattr/cfmakeraw
 * on fd 0/1, and it contains NO fork/exec/openpty/dup2.
 *
 * WHO ECHOES (rd vms-a353). In CTERM the TERMINAL echoes what is typed, as the
 * host's Start Read says (AA-DY88A-TK 4.16.3: N = no echo, T = echo the
 * terminator); the host never sends the typed text back -- in the oracle no
 * host->client frame carries the typed username or password, and a real VAX
 * client shows "Username: SYSTEM" but a blank "Password: " (docs/oracle/
 * vax-sethost-cterm.*; tests/lab/captures/decnet-sethost-inbound-20261005/
 * vax1-console.txt). So each Start Read becomes ONE local terminal read on the
 * TT: channel, issued by the executive terminal driver with the Start Read's
 * echo: IO$M_NOECHO when N is set, IO$M_TRMNOECHO when T is clear. The driver
 * -- not this program, not the substrate tty -- echoes and edits the line,
 * exactly as it does for a local LOGINOUT. (The earlier design asked for
 * IO$K_TT_PASSALL on the SYS$INPUT channel and relied on the remote for echo;
 * that channel carries no executive terminal channel, so the request was a
 * silent no-op, the line discipline echoed every read, and the remote
 * Password: was shown in clear on the originating terminal.)
 */

static void sethost_mkdesc(struct dsc$descriptor_s *d, const char *s)
{
    d->dsc$w_length = (uint16_t)strlen(s);
    d->dsc$b_dtype = DSC$K_DTYPE_T;
    d->dsc$b_class = DSC$K_CLASS_S;
    d->dsc$a_pointer = (char *)s;
}

/* Write a NUL-terminated string to the local terminal through its VMS output
 * channel ($QIO IO$_WRITEVBLK). `record` != 0 writes it as ONE VMS RECORD
 * (carriage control " ", P4 = 0x20): the terminal driver puts it on a line of
 * its own -- a new line before it, rendered for wherever the cursor is, and a
 * carriage return after -- the way VMS writes a message to a terminal. */
static void sethost_term_write(uint16_t chan, const char *s, int record)
{
    struct _iosb iosb;
    fflush(stdout);
    (void)sys$qiow(0, chan, IO$_WRITEVBLK, &iosb, NULL, 0,
                   (void *)s, (uint32_t)strlen(s), 0, record ? 0x20 : 0, 0, 0);
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
        /* rd vms-985: what the RUN link owes the wire -- retransmissions after
         * a NAK or timeout, data the peer's flow control held, link services. */
        for (unsigned g = 0; g < 2u * DNET_LINK_TXQ + 4u; g++) {
            thas = 0;
            if (dnet_engine_link_poll(eng, now, frame, sizeof(frame), &tlen, &thas, NULL)
                    != DNET_ENGINE_OK || !thas)
                break;
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
    int rc = dnet_engine_link_send(eng, pdu, plen, frame, sizeof(frame), &flen, now);
    if (rc == DNET_ENGINE_QUEUED)
        return 0;                 /* held by the peer's flow control; the pump sends it */
    if (rc != DNET_ENGINE_OK)
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
 * or the run ends. It waits on the datalink receive (the executive's bounded
 * timeout) and asks the terminal driver for type-ahead (rd vms-d3cd) -- no poll()
 * on host descriptors -- so the HELLO cadence + link tick keep firing. On teardown control returns with the canonical %REM-S-END.
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
    int read_pending, session_bound_ever, stdin_eof, rc, done;
    int skip_lf;                        /* CTERM Write "newline" flag state  */
    int quiet;                          /* a user's SET HOST: no DECNETD-I chatter */
    int rd_echo, rd_term_echo;          /* the pending Start Read's echo (vms-a353) */
    uint16_t ch_in, ch_out;
    uint16_t ch_tt;                     /* TT:, when SYS$INPUT is the terminal; 0 = not */
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

/* Show what the host wrote. On a terminal the bytes go to the terminal unit
 * as they are ($QIO IO$_WRITEVBLK, no carriage control): the host already
 * formatted them for a terminal, and the driver tracks where they leave the
 * cursor -- which is what puts %REM-S-END on a line of its own (rd vms-a353).
 * Off a terminal they go to SYS$OUTPUT. */
static void sethost_show(struct sh_ctx *x, const uint8_t *b, size_t n)
{
    struct _iosb iosb;
    (void)sys$qiow(0, x->ch_tt ? x->ch_tt : x->ch_out, IO$_WRITEVBLK, &iosb, NULL, 0,
                   (void *)b, (uint32_t)n, 0, 0, 0, 0);
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
                sethost_show(x, shown, sn);
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
        if (txtlen)
            sethost_show(x, txt, txtlen);
    } else if (tk == DNET_CTERM_TK_START_READ) {
        /* rd vms-6165: a 02-08 is PROMPT-AND-READ -- display the prompt text,
         * THEN answer with exactly one input line. One solicit -> one line,
         * never more, never before this arrives.
         *
         * rd vms-a353: the read's echo is the HOST's to choose (N / T flags);
         * a flags field that does not parse reads as no-echo. */
        x->term.writes_recv++;
        (void)dnet_cterm_start_read_echo(data, len, &x->rd_echo, &x->rd_term_echo);
        if (txtlen)
            sethost_show(x, txt, txtlen);
        if (x->ch_tt) {
            /* A TERMINAL: the line is read NOW, by the terminal driver, with
             * this read's echo (sethost_on_term_input). Type-ahead typed
             * before the solicit waits, unechoed, in the driver until then --
             * so a password typed early is never echoed by an earlier read. */
            x->read_pending = 1;
            return;
        }
        /* Not a terminal (a pipe or a file): nothing is echoed; the lines are
         * queued as they arrive and one is released per solicit. */
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

/* Does the client want local input now? On a terminal only while a host
 * Start Read is outstanding (the driver keeps type-ahead until then); from a
 * pipe whenever it is readable (lines are queued, released per solicit). */
static int sethost_wants_input(const struct sh_ctx *x)
{
    if (!dnet_cterm_is_bound(&x->term) || x->stdin_eof)
        return 0;
    return x->ch_tt ? x->read_pending : 1;
}

/* Satisfy the outstanding Start Read from the TERMINAL: one read by the
 * executive terminal driver, echoing exactly as the Start Read asked (rd
 * vms-a353), answered with one Read Data carrying the line and the terminator
 * the driver ended it with. */
static void sethost_read_terminal_line(struct sh_ctx *x)
{
    uint8_t line[DNET_CTERM_MAX_DATA];
    struct _iosb iosb;
    uint32_t func = IO$_READVBLK;

    if (!x->rd_echo)
        func |= IO$M_NOECHO;
    if (!x->rd_term_echo)
        func |= IO$M_TRMNOECHO;
    memset(&iosb, 0, sizeof iosb);
    uint32_t rst = sys$qiow(0, x->ch_tt, func, &iosb, NULL, 0,
                            line, (uint32_t)sizeof(line), 0, 0, 0, 0);
    uint32_t est = (rst & 1) ? iosb.iosb$w_status : rst;
    if (!(est & 1)) {
        /* A read the driver aborted (CTRL/Y) leaves the solicit outstanding
         * for the next line. Anything else -- the terminal hung up, its port
         * is gone -- stops reading it but keeps the link: the session still
         * ends on the host's Unbind or a link drop, never on the local side
         * by itself (rd vms-6165). */
        if (est != SS$_ABORT)
            x->stdin_eof = 1;
        return;
    }
    size_t n = iosb.iosb$w_bcnt;
    if (n > sizeof line)
        n = sizeof line;
    /* IOSB words 2/3: the terminator character and its size. A read that ended
     * without one (the buffer filled) is sent with CR, as a full line. */
    uint8_t term = (iosb.iosb$l_dev_depend >> 16) ? (uint8_t)(iosb.iosb$l_dev_depend & 0xff)
                                                  : 0x0d;
    size_t clen = 0;
    if (dnet_cterm_found_read_data_build(line, n, term, x->cpdu, sizeof x->cpdu,
                                         &clen) != 0 ||
        sh_link_send(x, x->cpdu, clen) != 0) {
        fprintf(stderr, "DECNETD-E-READDATA, could not send CTERM Read Data\n");
        x->rc = 1; x->done = 1;
        return;
    }
    x->term.reads_sent++;
    x->read_pending = 0;
}

/* The local terminal is readable: read it through its VMS channel. */
static void sethost_on_term_input(struct sh_ctx *x)
{
    uint8_t inbuf[DNET_CTERM_MAX_DATA];
    struct _iosb iosb;
    if (x->ch_tt) {
        if (x->read_pending)
            sethost_read_terminal_line(x);
        return;
    }
    /* Read the keystrokes through the VMS terminal channel ($QIO), never a raw
     * read on fd 0. */
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

/*
 * WAITING THE VMS WAY (rd vms-d3cd). The SET HOST client used to poll() the
 * substrate descriptor behind its terminal channel (and, standalone, the
 * datalink). It now asks the TERMINAL DRIVER: $QIOW IO$_SENSEMODE!
 * IO$M_TYPEAHDCNT on the TT: channel says whether a line has been started in
 * the type-ahead buffer, and the client then reads it with $QIOW, exactly as
 * the poll used to gate it. Between turns it waits on a timer event flag
 * ($SETIMR + $WAITFR), never nanosleep/poll on a host descriptor.
 */
#define DECNETD_WAIT_EFN 22u    /* a local event flag (1..23 are the image's) */

static void decnetd_wait_ms(unsigned ms)
{
    /* a VMS delta time: negative, in 100 ns units */
    uint64_t delta = (uint64_t)(-(int64_t)ms * 10000);
    if (sys$setimr(DECNETD_WAIT_EFN, &delta, NULL, 0, 0) & 1)
        (void)sys$waitfr(DECNETD_WAIT_EFN);
}

/* Is the local side ready to supply input? A TERMINAL (TT: channel): the
 * driver's type-ahead count is nonzero. A non-terminal SYS$INPUT (a file or
 * pipe a harness feeds): only when the host has solicited a read -- the
 * read then waits in $QIOW for the next record, which is what a VMS image
 * reading a file does. */
static int sethost_term_ready(struct sh_ctx *x)
{
    if (!x->ch_tt)
        return x->read_pending;
    uint8_t tc[8];
    struct _iosb iosb;
    memset(tc, 0, sizeof tc);
    memset(&iosb, 0, sizeof iosb);
    uint32_t st = sys$qiow(0, x->ch_tt, IO$_SENSEMODE | IO$M_TYPEAHDCNT, &iosb,
                           NULL, 0, tc, (uint32_t)sizeof tc, 0, 0, 0, 0);
    if (st & 1)
        st = iosb.iosb$w_status;
    if (!(st & 1))
        return 1;      /* the driver cannot say: let the read report it */
    return (tc[0] | (tc[1] << 8)) != 0;
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
    /* rd vms-a353: when SYS$INPUT is the job's terminal, read it through a
     * channel to the TERMINAL UNIT (TT:) -- the executive terminal driver's
     * channel, on which a read takes IO$M_NOECHO. Without it a remote
     * Password: read would be echoed, so a terminal whose channel cannot be
     * had refuses SET HOST rather than show a password. */
    {
        int fd = vms$$chan_to_fd(x->ch_in);
        if (fd >= 0 && isatty(fd)) {
            struct dsc$descriptor_s dtt;
            sethost_mkdesc(&dtt, "TT:");
            uint32_t st = sys$assign(&dtt, &x->ch_tt, 0, NULL);
            if (!(st & 1)) {
                fprintf(stderr, "DECNETD-E-NOTERMCHAN, could not $ASSIGN the"
                                " terminal (TT:), status %u\n", st);
                x->ch_tt = 0;
                sys$dassgn(x->ch_in); sys$dassgn(x->ch_out);
                x->ch_in = x->ch_out = 0;
                return -1;
            }
        }
    }
    return 0;
}

static void sethost_close_terminal(struct sh_ctx *x)
{
    if (x->ch_tt) sys$dassgn(x->ch_tt);
    sys$dassgn(x->ch_in);
    sys$dassgn(x->ch_out);
    x->ch_tt = x->ch_in = x->ch_out = 0;
}

/* Restore the terminal, unbind a bound session, say %REM-S-END. The caller
 * then releases the link. */
static void sethost_finish(struct sh_ctx *x, const char *local_node)
{
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
        if (x->ch_tt) {
            /* rd vms-a353: ON ITS OWN LINE, as VMS shows it -- "SYSTEM logged
             * out at ..." then "%REM-S-END, ..." beneath it. The remote's last
             * output ends with a bare CR (its line feed owed), so the message
             * goes as a record: the driver starts it on a new line rendered
             * for where the cursor is (LF after a CR, CR LF mid-line). */
            snprintf(msg, sizeof(msg),
                     "%%REM-S-END, control returned to node %s::", local_node);
            sethost_term_write(x->ch_tt, msg, 1);
        } else {
            snprintf(msg, sizeof(msg),
                     "%%REM-S-END, control returned to node %s::\n", local_node);
            sethost_term_write(x->ch_out, msg, 0);
        }
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
    /* The wire wait: a frame, or 20 ms (then the terminal is looked at). */
    (void)scs_datalink_set_recv_timeout_ms(sock, 20);

    uint8_t frame[DNET_FRAME_MAX], rxbuf[DNET_FRAME_MAX];
    size_t flen = 0;
    dnet_tick_t now = monotonic_sec();
    if (dnet_engine_link_open(eng, parea, pnode, dnet_client_lla(), sc, sclen, 1459, 1,
                              DNET_NSP_VER_41, frame, sizeof(frame), &flen, now)
            != DNET_ENGINE_OK) {
        fprintf(stderr, "DECNETD-E-NOCONNECT, could not open a logical link"
                        " to %u.%u\n", parea, pnode);
        sethost_close_terminal(&x);
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

        /* rd vms-d3cd: the datalink receive itself is the wait (bounded by
         * the executive's receive timeout set above), then the terminal
         * driver is asked for type-ahead -- no poll() on host descriptors. */
        {
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

        if (!x.done && sethost_wants_input(&x) && sethost_term_ready(&x))
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
    sethost_close_terminal(&x);
    return x.rc;
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

/*
 * ONE NETACP PER NODE (rd vms-365). A node runs one network ACP: it owns the
 * node's datalink, its logical-link pool and the _NET: request mailbox. A
 * second no-argv DECNETD (a RUN/DETACHED of the image under any process name
 * other than NETACP, which the executive already refuses -DUPLNAM) used to
 * come up as a second full NETACP beside the first -- a second pool and a
 * second engine racing the first for every frame, and on a swapless VAX the
 * memory that left the next fork with none (PRCLOST). It is LIVE, not merely
 * published, when DNET$NETACP_REQ names a mailbox that still exists: NETACP's
 * request mailbox is temporary, so it goes with the process, and a logical a
 * dead NETACP left behind must not block the next STARTNET. Anything but
 * "no such device" (a protection refusal included) means it exists.
 * Returns 1 live, 0 none, -1 no executive to ask.
 */
static int netacp_live(void)
{
    char dev[64];
    uint16_t dl = 0;
    int r = vms_kif_lnm_translate(VMS_LNM_TBL_SYSTEM, NETACP_REQ_LOGNAM, 0, dev,
                                  sizeof dev - 1, &dl, NULL, NULL);
    if (r < 0) return -1;
    if (r != 1 || dl == 0) return 0;
    dev[dl] = '\0';
    if (!vms_pcb_get())
        vms_pcb_init(0);
    struct dsc$descriptor_s d;
    sethost_mkdesc(&d, dev);
    uint16_t chan = 0;
    uint32_t st = sys$assign(&d, &chan, 0, NULL);
    if (st & 1) {
        sys$dassgn(chan);
        return 1;
    }
    return (st == SS$_NOSUCHDEV || st == SS$_IVDEVNAM) ? 0 : 1;
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

    char ncb[64];
    int n = snprintf(ncb, sizeof ncb, "%s::\"%d=\"", target, DNET_CTERM_OBJECT);
    size_t xf = 0;
    uint32_t st = (n > 0 && (size_t)n < sizeof ncb)
        ? netcli_op(c, DNET_BROKER_OP_OPEN, ncb, (size_t)n, NULL, 0, &xf) : SS$_BADPARAM;
    if (!(st & 1)) {
        /* VMS SET HOST reports only the reason the link failed -- the
         * condition's text from the system message catalog ($GETMSG), so
         * SS$_NOSUCHNODE reads "%SYSTEM-F-NOSUCHNODE, remote node is unknown"
         * and not "%SYSTEM-F-0051, message number 652" (rd vms-5843). */
        char why[256] = "";
        uint16_t wlen = 0;
        struct dsc$descriptor_s wd;
        wd.dsc$w_length = (uint16_t)(sizeof why - 1);
        wd.dsc$b_dtype = DSC$K_DTYPE_T;
        wd.dsc$b_class = DSC$K_CLASS_S;
        wd.dsc$a_pointer = why;
        if (sys$getmsg(st, &wlen, &wd, 15, NULL) & 1)
            why[wlen] = '\0';
        else
            why[0] = '\0';
        printf("%s\n", why[0] ? why : "%SYSTEM-F-ABORT, abort");
        fflush(stdout);
        sethost_close_terminal(&x);
        /* The message is said once, here; $STATUS carries the condition with
         * STS$M_INHIB_MSG, as a VMS command that already reported its failure
         * leaves it. A bare return 1 made DCL add "%DCL-E-ABORT, image
         * SYS$SYSTEM:DECNETD.EXE exited with error status %X00000001" under
         * the real reason -- an image name a SET HOST user never ran (rd
         * vms-5843). */
        sys$exit(st | 0x10000000u);
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
        /* rd vms-d3cd: ask the terminal driver for type-ahead, then wait on
         * a timer event flag -- never poll() the substrate descriptor. */
        if (sethost_wants_input(&x) && sethost_term_ready(&x))
            sethost_on_term_input(&x);
        else
            decnetd_wait_ms(20);
    }
    sethost_finish(&x, local_node);
    (void)netcli_op(c, DNET_BROKER_OP_CLOSE, NULL, 0, NULL, 0, &xf);
    sethost_close_terminal(&x);
    return x.rc;
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
 * slot. Inbound data is queued per link until the client's READVBLK takes it.
 * The queue is the link's NSP receive window (rd vms-985): the link asks the
 * peer to stop (link service FCMOD 1) when half of it is used and to resume
 * (FCMOD 2) when it drains, and a segment that still finds it full is NAKed
 * unacknowledged so the peer resends it. A slow reader slows the peer down; it
 * never costs the link (before vms-985 the 33rd queued segment aborted the
 * link with Disconnect Initiate reason 1 and the client saw -RMS-F-RER). */
#define NETACP_OUT_RXQ        32
#define NETACP_OUT_IDLE_SECS  60
/* How long a closing link may wait for its peer to take the queued data (the
 * peer may hold it stopped) before NETACP aborts it. OVMX bound, labelled. */
#define NETACP_LINGER_SECS    120
static unsigned long g_netacp_rxq_overrun;   /* must stay 0: the window prevents it */

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
    /* rd vms-985: a normal close waiting for the link's queued data to be
     * acknowledged before its Disconnect Initiate goes (NSP). */
    int      closing;
    const char *closing_why;
    dnet_tick_t closing_deadline;
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

static void netacp_slot_end(struct netacp_slot *sl, int sock, unsigned ifindex,
                            dnet_tick_t now, const char *why);

/* Put on the wire everything the slot's NSP link owes it now (rd vms-985):
 * flow-control link services, retransmissions after a NAK or a timeout, and
 * data the peer's flow control had held. A link whose retransmit budget ran
 * out is dead: an outbound link reports it to its process (ABORT on the next
 * request), an inbound session ends. */
static void netacp_link_flush(struct netacp_slot *sl, int sock, unsigned ifindex,
                              dnet_tick_t now)
{
    if (!sl->used || !sl->lk.link_active)
        return;
    uint8_t fr[DNET_FRAME_MAX];
    for (unsigned guard = 0; guard < 2u * DNET_LINK_TXQ + 4u; guard++) {
        size_t fn = 0; int has = 0;
        enum dnet_link_event ev = DNET_LINK_EV_NONE;
        if (dnet_engine_link_poll(&sl->lk, now, fr, sizeof fr, &fn, &has, &ev) !=
            DNET_ENGINE_OK)
            return;
        if (ev == DNET_LINK_EV_DISCONNECT) {
            sl->lk.link_active = 0;
            sl->disc_reason = sl->lk.link.disc_reason;
            if (sl->outbound) {
                sl->remote_gone = 1;
                sl->rxq_count = 0;
            } else {
                netacp_slot_end(sl, sock, ifindex, now,
                                "the remote stopped acknowledging (retransmit limit)");
            }
            return;
        }
        if (!has)
            break;
        netacp_send(sock, ifindex, sl->peer_mac, fr, fn);
    }
    /* A normal close whose queue has drained: its DI just went out. */
    if (sl->used && sl->closing && !dnet_link_is_up(&sl->lk.link))
        netacp_slot_end(sl, sock, ifindex, now, sl->closing_why);
}

/* Ship every CTERM segment the host FSM has queued, in order, over the link,
 * as far as the link's transmit queue takes them (the rest wait in the FSM). */
static void netacp_cterm_drain(struct netacp_slot *sl, int sock, unsigned ifindex,
                               dnet_tick_t now)
{
    uint8_t seg[DNET_CTH_SEG_MAX], fr[DNET_FRAME_MAX];
    size_t n = 0, fn = 0;
    while (dnet_link_tx_room(&sl->lk.link) > 0 &&
           dnet_cth_tx_pop(&sl->cth, seg, sizeof seg, &n))
        if (dnet_engine_link_send(&sl->lk, seg, n, fr, sizeof fr, &fn, now) == DNET_ENGINE_OK)
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
    if (sl->closing) {
        /* Second pass: the queue drained and the DI went (or the linger bound
         * ran out / the link died -- then abort what is left). */
        sl->closing = 0;
        if (dnet_link_is_up(&sl->lk.link) &&
            dnet_engine_link_close(&sl->lk, DNET_LINK_REASON_NORMAL, fr, sizeof fr, &fn,
                                   now) == 0)
            netacp_send(sock, ifindex, sl->peer_mac, fr, fn);
        sl->lk.link_active = 0;
        goto release;
    }
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
    netacp_link_flush(sl, sock, ifindex, now);   /* what flow control lets go */
    if (!sl->used) return;
    if (dnet_link_is_up(&sl->lk.link)) {
        /* NSP normal close: the DI waits until every queued segment is sent
         * and acknowledged, so the tail of a transfer is never lost. */
        int crc = dnet_engine_link_close_normal(&sl->lk, DNET_LINK_REASON_NORMAL, fr,
                                                sizeof fr, &fn, now);
        if (crc == DNET_ENGINE_OK) {
            netacp_send(sock, ifindex, sl->peer_mac, fr, fn);
        } else if (crc == DNET_ENGINE_QUEUED) {
            sl->closing = 1;
            sl->closing_why = why;
            sl->closing_deadline = now + NETACP_LINGER_SECS;
            return;                        /* netacp_link_flush finishes it */
        }
    }
release:
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
        netacp_link_flush(sl, sock, ifindex, now);
        if (!sl->used) continue;
        live++;
        if (sl->closing) {
            if (now >= sl->closing_deadline || !sl->lk.link_active)
                netacp_slot_end(sl, sock, ifindex, now, sl->closing_why);
            continue;                      /* lingering: only its link runs */
        }
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
            /* Take the server's next message only when the link can queue it:
             * a peer's flow control backs up into the server, never drops. */
            r = 0;
            while (dnet_link_tx_room(&sl->lk.link) > 0 &&
                   (r = dnet_fal_proc_poll(&sl->fal, buf, sizeof buf, &n, &xst)) == 1) {
                if (dnet_engine_link_send(&sl->lk, buf, n, fr, sizeof fr, &fn, now) ==
                    DNET_ENGINE_OK)
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
    const char *user = strstr(hs->remote_port_info, "::");
    const struct dnet_nodedb *db = netacp_nodedb(NULL);
    if (!user || !hs->devnam[0] || !db)
        return;
    const struct dnet_node_entry *e = dnet_nodedb_by_addr(db, peer);
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
    const struct dnet_node_entry *e = NULL;
    const struct dnet_nodedb *db = netacp_nodedb(NULL);
    if (db)
        e = dnet_nodedb_by_addr(db, peer);
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

/*
 * NETACP's VOLATILE OBJECT DATABASE (rd vms-2d0): the Session Control objects
 * its inbound-connect dispatch serves. ONE table: the dispatch gate
 * (netacp_object_served, in netacp_dispatch_frame) and the NCP SHOW KNOWN
 * OBJECTS answer (netacp_show) both read it, so SHOW can never list an object a
 * connect would be refused, nor omit one that is served. An object is served
 * either by ACTIVATING an image per connect (file set -- FAL: a FAL.EXE server
 * process as the authenticated user, rd vms-d85) or IN PLACE by NETACP itself
 * (file NULL -- CTERM: NETACP's own CTERM host mints the RTAn: session, so SHOW
 * prints NETACP's PID, as a real VAX prints REMACP's for its CTERM). The
 * permanent NETOBJECT.DAT (NCP DEFINE / LIST OBJECT) does not yet feed this
 * table -- a later rung; LIST reads that file, SHOW reads this.
 */
struct netacp_object {
    const char *name;
    int         number;
    const char *file;         /* activated image; NULL = served in NETACP itself */
};
static const struct netacp_object g_netacp_objects[] = {
    { "FAL",   DNET_FAL_OBJECT,    DNET_FAL_IMAGE_SPEC },
    /* MAIL-11 (rd vms-47fd): a MAIL_SERVER.EXE process under the MAIL object's
     * account; the oracle VAX lists it as MAIL 27 MAIL_SERVER.EXE. */
    { "MAIL",  DNET_MAIL11_OBJECT, DNET_MAILP_IMAGE_SPEC },
    { "CTERM", DNET_CTERM_OBJECT,  NULL },
};
#define NETACP_NOBJECTS ((int)(sizeof g_netacp_objects / sizeof g_netacp_objects[0]))

/* The table row for an inbound connect to `number`, or NULL (refused OBJREJ).
 * A NETACP not serving inbound (--no-cterm-server) serves no object at all. */
static const struct netacp_object *netacp_object_served(int number)
{
    if (!g_netacp_serve_inbound)
        return NULL;
    for (int i = 0; i < NETACP_NOBJECTS; i++)
        if (g_netacp_objects[i].number == number)
            return &g_netacp_objects[i];
    return NULL;
}

/* The datalink under NETACP's circuit is open: main() sets it once the raw L2
 * socket is bound (NETACP exits on any datalink failure, so a serving NETACP
 * has it); the host selftest sets it for its in-process pool. */
static int g_netacp_circuit_on;

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

/* rd vms-3106: is `addr` the local address of a link a pool slot holds now? */
static int netacp_lla_in_use(uint16_t addr, void *ctx)
{
    const struct netacp_slot *slots = ctx;
    for (int i = 0; i < NETACP_MAX_SESSIONS; i++)
        if (slots[i].used && slots[i].lk.link.local_addr == addr)
            return 1;
    return 0;
}

/* The next logical-link address for a new pool link. It wraps within
 * [DNET_LLA_MIN, 0xFFFF] and never hands out an address a live slot holds. */
static uint16_t netacp_take_lla(const struct netacp_slot *slots, uint16_t *next_lla)
{
    return dnet_lla_take(next_lla, netacp_lla_in_use, (void *)slots);
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

/*
 * netacp_netctl - NCP SET / CLEAR NODE against this NETACP's VOLATILE node
 * database (dnet_netctl request; NCP DEFINE / PURGE change the permanent file).
 * VMS's NCP SET and CLEAR need OPER, so the requester's current privileges are
 * read from the executive (its PID is the one that queued the request) and a
 * requester without OPER is refused SS$_NOPRIV -- as is one the executive
 * cannot describe. The answer is a status only.
 */
/* The requester's current privileges, as the executive holds them; 0 when it
 * cannot say (fail closed). A hook so the in-process self-test, which has no
 * executive, can name a requester's privileges. */
static uint64_t netacp_requester_privs_exec(uint32_t pid)
{
    struct vms_procinfo who;
    memset(&who, 0, sizeof who);
    if (!(vms_kif_getjpi_pid(pid, &who) & 1))
        return 0;
    return who.cur_privs;
}
static uint64_t (*g_netacp_requester_privs)(uint32_t pid) = netacp_requester_privs_exec;

static void netacp_netctl(const struct dnet_broker_req *req)
{
    struct dnet_netctl_req q;
    uint32_t st;

    if (dnet_netctl_req_decode(req->data, req->datalen, &q) != DNET_NETSHOW_OK) {
        netacp_respond(NULL, req->reply_unit, req->corr_id, SS$_BADPARAM, NULL, 0);
        return;
    }
    if (!(g_netacp_requester_privs(req->owner_pid) & PRV$M_OPER)) {
        netacp_respond(NULL, req->reply_unit, req->corr_id, SS$_NOPRIV, NULL, 0);
        return;
    }
    if (!g_vnodes_live)
        netacp_vnodes_load();
    int rc;
    if (q.kind == DNET_NETCTL_SET_NODE)
        rc = q.addr ? dnet_nodedb_set(&g_vnodes, q.addr, q.name) : DNET_NODEDB_EINVAL;
    else if (q.addr)
        rc = dnet_nodedb_clear_addr(&g_vnodes, q.addr);
    else
        rc = dnet_nodedb_clear_name(&g_vnodes, q.name);
    st = rc == DNET_NODEDB_OK     ? SS$_NORMAL
       : rc == DNET_NODEDB_ENOENT ? SS$_NOSUCHNODE
       : rc == DNET_NODEDB_EFULL  ? SS$_INSFMEM
       :                            SS$_BADPARAM;
    netacp_respond(NULL, req->reply_unit, req->corr_id, st, NULL, 0);
}

static void netacp_show(const struct netacp_slot *slots, const struct dnet_engine *node,
                        const struct dnet_broker_req *req)
{
    static struct dnet_netshow_rsp r;
    static struct dnet_netshow_node all[DNET_NODEDB_MAX + DNET_ADJ_MAX_NEIGHBORS];
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

    const struct dnet_nodedb *vdb = netacp_nodedb(NULL);   /* the VOLATILE database */
    if (q.entity == DNET_NETSHOW_ENT_NODES) {
        unsigned n = netacp_show_nodes(slots, node, vdb, all,
                                       (unsigned)(sizeof all / sizeof all[0]));
        r.total = (uint16_t)n;
        r.first = q.cursor > n ? (uint16_t)n : q.cursor;
        unsigned c = n - r.first;
        r.count = (uint8_t)(c > DNET_NETSHOW_NODES_PER ? DNET_NETSHOW_NODES_PER : c);
        memcpy(r.u.node, &all[r.first], r.count * sizeof all[0]);
    } else if (q.entity == DNET_NETSHOW_ENT_CIRCUITS || q.entity == DNET_NETSHOW_ENT_LINES) {
        /* ONE circuit and ONE line: the datalink this NETACP runs (multi-NIC
         * circuits are a later rung). Named from the engine (device-native:
         * the executive's ETH0: -> ETH-0); state = its datalink is open. */
        r.total = 1;
        r.first = q.cursor > 1 ? 1 : q.cursor;
        r.count = (uint8_t)(1 - r.first);
        if (r.count && q.entity == DNET_NETSHOW_ENT_CIRCUITS) {
            struct dnet_netshow_circuit *c = &r.u.circ[0];
            snprintf(c->name, sizeof c->name, "%s", node->circuit);
            c->state_on = g_netacp_circuit_on ? 1 : 0;
            /* The adjacent ROUTING node: an endnode's selected designated
             * router. A routing NETACP's adjacency table does not record which
             * neighbours route, so it names none (INV-6), never a guess. */
            if (!dnet_engine_is_router(node) && node->have_dr) {
                c->adj_addr = dnet_addr_from_id(node->dr_id);
                netacp_show_name(vdb, c->adj_addr, c->adj_name);
            }
        } else if (r.count) {
            struct dnet_netshow_line *l = &r.u.line[0];
            dnet_engine_line_name(node, l->name, sizeof l->name);
            l->state_on = g_netacp_circuit_on ? 1 : 0;
        }
    } else if (q.entity == DNET_NETSHOW_ENT_OBJECTS) {
        static struct dnet_netshow_object objs[NETACP_NOBJECTS];
        unsigned n = 0;
        uint32_t self_pid = 0;
        struct vms_procinfo me;
        memset(&me, 0, sizeof me);
        if (vms_kif_getjpi_self(&me) & 1)
            self_pid = me.vms_pid;            /* no executive row: no PID shown */
        for (int i = 0; i < NETACP_NOBJECTS; i++) {
            const struct netacp_object *t = netacp_object_served(g_netacp_objects[i].number);
            if (!t)
                continue;
            struct dnet_netshow_object *o = &objs[n++];
            memset(o, 0, sizeof *o);
            snprintf(o->name, sizeof o->name, "%s", t->name);
            o->number = (uint8_t)t->number;
            if (t->file) {
                /* NCP prints the file as the database holds it: the name, the
                 * SYS$SYSTEM: default it is activated from left off. */
                const char *f = strrchr(t->file, ':');
                snprintf(o->file, sizeof o->file, "%s", f ? f + 1 : t->file);
            } else {
                o->pid = self_pid;
            }
        }
        /* SHOW order: by number, then name (the oracle's order). */
        for (unsigned a = 1; a < n; a++)
            for (unsigned b = a; b > 0 && (objs[b - 1].number > objs[b].number ||
                 (objs[b - 1].number == objs[b].number &&
                  strcmp(objs[b - 1].name, objs[b].name) > 0)); b--) {
                struct dnet_netshow_object sw = objs[b];
                objs[b] = objs[b - 1];
                objs[b - 1] = sw;
            }
        r.total = (uint16_t)n;
        r.first = q.cursor > n ? (uint16_t)n : q.cursor;
        unsigned c = n - r.first;
        r.count = (uint8_t)(c > DNET_NETSHOW_OBJS_PER ? DNET_NETSHOW_OBJS_PER : c);
        memcpy(r.u.obj, &objs[r.first], r.count * sizeof objs[0]);
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
            netacp_show_name(vdb, sl->peer, l->name);
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
        /* The link accepted (and acknowledged) this segment only because its
         * receive window -- this queue's free entries -- had room. */
        if (sl->rxq_count < NETACP_OUT_RXQ) {
            unsigned t = (sl->rxq_head + sl->rxq_count) % NETACP_OUT_RXQ;
            uint16_t n = sl->lk.rx_datalen > DNET_NSP_MAX_DATA ? DNET_NSP_MAX_DATA
                                                               : sl->lk.rx_datalen;
            memcpy(sl->rxq[t], sl->lk.rx_data, n);
            sl->rxq_len[t] = n;
            sl->rxq_count++;
        } else {
            g_netacp_rxq_overrun++;        /* unreachable by construction; counted */
        }
        dnet_link_rx_room(&sl->lk.link, NETACP_OUT_RXQ - sl->rxq_count);
        netacp_link_flush(sl, sock, ifindex, now);
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

    if (base == DNET_BROKER_OP_SHOW) {          /* NCP SHOW / SET|CLEAR NODE  */
        if (dnet_netctl_is_req(req->data, req->datalen))
            netacp_netctl(req);
        else
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
        uint16_t lla = netacp_take_lla(slots, next_lla);
        uint8_t fr[DNET_FRAME_MAX]; size_t fn = 0;
        int orc = dnet_engine_link_open(&sl->lk, ra, rn, lla, desc, dlen, 1459, 1,
                                        NETACP_NSP_VERSION, fr, sizeof fr, &fn, now);
        memset(desc, 0, sizeof desc);      /* it carried the access-control password */
        if (orc != DNET_ENGINE_OK) {
            netacp_respond(NULL, req->reply_unit, req->corr_id, SS$_ABORT, NULL, 0);
            memset(sl, 0, sizeof *sl);
            return;
        }
        /* The client's receive queue is this link's NSP receive window. */
        dnet_link_rx_window(&sl->lk.link, NETACP_OUT_RXQ);
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
        {
            int src = dnet_engine_link_send(&sl->lk, req->data, req->datalen, fr, sizeof fr,
                                            &fn, now);
            if (src == DNET_ENGINE_EAGAIN) {
                /* The peer has not acknowledged enough yet (or has stopped us):
                 * nothing taken; the client offers the message again, as a
                 * waiting READVBLK asks again (rd vms-985). */
                netacp_respond(sl, sl->reply_unit, req->corr_id, SS$_ENDOFFILE, NULL, 0);
                return;
            }
            if (src != DNET_ENGINE_OK && src != DNET_ENGINE_QUEUED) {
                netacp_respond(sl, sl->reply_unit, req->corr_id, SS$_ABORT, NULL, 0);
                return;
            }
            if (src == DNET_ENGINE_OK)
                netacp_send(sock, ifindex, sl->peer_mac, fr, fn);
        }
        netacp_respond(sl, sl->reply_unit, req->corr_id, SS$_NORMAL, NULL, 0);
        return;

    case DNET_BROKER_OP_RECV:
        if (sl->rxq_count) {
            unsigned h = sl->rxq_head;
            netacp_respond(sl, sl->reply_unit, req->corr_id, SS$_NORMAL,
                           sl->rxq[h], sl->rxq_len[h]);
            sl->rxq_head = (h + 1) % NETACP_OUT_RXQ;
            sl->rxq_count--;
            if (!sl->remote_gone) {
                /* Room again: the link may now tell the peer to resume. */
                dnet_link_rx_room(&sl->lk.link, NETACP_OUT_RXQ - sl->rxq_count);
                netacp_link_flush(sl, sock, ifindex, now);
            }
            return;
        }
        netacp_respond(sl, sl->reply_unit, req->corr_id,
                       sl->remote_gone ? SS$_ABORT : SS$_ENDOFFILE, NULL, 0);
        return;

    case DNET_BROKER_OP_CLOSE:
        /* A normal disconnect: its DI follows the link's queued data, which
         * still goes out and is acknowledged (rd vms-985; the slot lingers
         * with no owner until then, bounded by NETACP_LINGER_SECS). */
        netacp_respond(sl, sl->reply_unit, req->corr_id, SS$_NORMAL, NULL, 0);
        if (sl->remote_gone || !dnet_link_is_up(&sl->lk.link)) {
            sl->lk.link_active = 0;
            netacp_outbound_release(sl, "its process disconnected");
            return;
        }
        sl->handle = 0;                    /* no request can name it any more */
        netacp_slot_end(sl, sock, ifindex, now, "its process disconnected");
        (void)fr; (void)fn;
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
        /* An ACK, NAK or link service may have released or rewound data. */
        if (ev != DNET_LINK_EV_DISCONNECT && ev != DNET_LINK_EV_DISCONNECT_CONF)
            netacp_link_flush(sl, sock, ifindex, now);
        if (!sl->used) return;
        if (sl->closing) {
            if (ev == DNET_LINK_EV_DISCONNECT || ev == DNET_LINK_EV_DISCONNECT_CONF) {
                sl->lk.link_active = 0;
                netacp_slot_end(sl, sock, ifindex, now, sl->closing_why);
            }
            return;                        /* closing: nothing more is delivered */
        }
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
    if (!netacp_object_served(obj)) {   /* NETACP's object table (rd vms-2d0) */
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
    uint16_t lla = netacp_take_lla(slots, next_lla);
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
/* Requests dropped because their writer could not READ the reply mailbox it
 * named (rd vms-046). */
static unsigned long g_broker_badreply;
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
 *
 * WHERE THE ANSWER GOES (rd vms-046). NETACP writes its answer into the
 * mailbox the request names (reply_unit), and NETACP's own privileges would
 * let that write land in ANY mailbox -- so a requester could aim NETACP's
 * writes at a mailbox it could not use itself (another process's reply
 * mailbox, or NETACP's own request mailbox). The executive is asked, for the
 * WRITER, whether it may READ that mailbox (VMS_IOCTL_MBX_CHKACC: the same
 * decision its own $QIO read would get, through vms_prot.h -- the shape of
 * $CHECK_ACCESS, docs/oracle/semantics/mbxown/); a requester that could not
 * read its answer is not answered at all.
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
        char rdev[24];
        snprintf(rdev, sizeof rdev, "MBA%u:", (unsigned)req.reply_unit);
        if (vms_kif_mbx_chkacc(rdev, sender_pid, VMS_MBX_ACC_READ) != SS$_NORMAL) {
            g_broker_badreply++;
            return;                        /* not a mailbox the requester can read */
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
 * rd vms-158d: what a real VMS node's operator sees when DECnet comes up --
 * NETACP's OPCOM message "DECnet starting" (tests/lab/captures/
 * vms-52d-0b01-oracle-20261013/armA-vax1-console.log: "Message from user
 * DECNET on VAX1 / DECnet starting"). Sent through $SNDOPR, so it is filed in
 * SYS$MANAGER:OPERATOR.LOG like any operator request; STARTNET.COM prints no
 * launch line of its own on the console. Best-effort: a refused $SNDOPR never
 * stops the network.
 */
static void netacp_opcom_starting(void)
{
    struct {
        struct opcdef hdr;
        char text[32];
    } msgbuf;
    static const char text[] = "DECnet starting";
    struct dsc$descriptor_s desc;

    memset(&msgbuf, 0, sizeof msgbuf);
    msgbuf.hdr.opc$b_ms_type = OPC$_RQ_RQST;
    OPC$SET_TARGET(msgbuf.hdr, OPC$M_NM_CENTRL);
    (void)snprintf(msgbuf.hdr.opc$l_ms_text, sizeof msgbuf.text, "%s", text);
    desc.dsc$a_pointer = (char *)&msgbuf.hdr;
    desc.dsc$w_length = (uint16_t)(OPC$K_MS_HDRLEN + sizeof text - 1);
    desc.dsc$b_dtype = DSC$K_DTYPE_T;
    desc.dsc$b_class = DSC$K_CLASS_S;
    (void)sys$sndopr(&desc, 0);
}

/*
 * ===================== the NETACP run (rd vms-0a7 S3) =====================
 * Everything NETACP does once it knows WHO it is. The shipped NETACP.EXE
 * (netacp_image_main below) gets that configuration the VMS way -- it takes NO
 * argv: NCP SET EXECUTOR STATE ON $CREPRCs it detached, and it reads its
 * executor from the node database through RMS and its LAN unit from the
 * executive. A test image (tests/vmsdecnet) that includes this file builds the
 * same structure from its own command line for a lab or host run.
 */
struct netacp_cfg {
    const char *ifname;       int ifname_explicit;
    const char *addr_s;
    const char *name;         int name_explicit;
    const char *device;
    const char *circuit;
    int hello_interval;
    int duration;
    int show_executor_only;
    int cterm_server;         int cterm_server_explicit;
    const char *set_host_to;  const char *set_host_user;
    int router_mode;          int router_priority;
    int detached;             /* the NETACP process NCP created: no command line */
};

static void netacp_cfg_default(struct netacp_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->ifname = DECNETD_DEFAULT_IFACE;
    c->name = "OVMX";
    /* Device-native naming (vms-47d): NETACP's datalink rides the primary NIC,
     * which the executive enters as ETH0: (src/kernel-core/vms_devtab.c
     * VMS_NIC_DEVNAM) -- so its circuit and line are ETH-0 (rd vms-2d0). */
    c->device = "ETH0";
    c->hello_interval = (int)DNET_T3_DEFAULT;
    /* The persistent node daemon SERVES inbound $ SET HOST (object 42) and file
     * access (object 17) by default: serving is what a DECnet ancillary
     * control process does, and it is dormant until a peer connects. */
    c->cterm_server = 1;
    c->set_host_user = "SYSTEM";
}

static int netacp_run(const struct netacp_cfg *cfg)
{
    const char *ifname = cfg->ifname;
    int ifname_explicit = cfg->ifname_explicit;
    const char *addr_s = cfg->addr_s;
    const char *name = cfg->name;
    int name_explicit = cfg->name_explicit;
    const char *device = cfg->device;
    const char *circuit = cfg->circuit;
    int hello_interval = cfg->hello_interval;
    int duration = cfg->duration;
    int show_executor_only = cfg->show_executor_only;
    int cterm_server = cfg->cterm_server;
    int cterm_server_explicit = cfg->cterm_server_explicit;
    const char *set_host_to = cfg->set_host_to;
    const char *set_host_user = cfg->set_host_user;
    int router_mode = cfg->router_mode;
    int router_priority = cfg->router_priority;

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
    if (set_host_to) {
        int nr = netacp_running();
        if (nr == 1 && !ifname_explicit) {
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

    /* ONE NETACP PER NODE (rd vms-365, netacp_live above). The persistent
     * daemon (no argv) that finds a NETACP already serving this node does not
     * become a second one: it says so and exits at once, before it opens a
     * datalink or touches its session pool. (Diagnostic runs with flags --
     * --iface lab runs, --show-executor -- are not NETACPs and are unaffected.) */
    if (cfg->detached && netacp_live() == 1) {
        fprintf(stderr, "DECNETD-E-NETACPACT, a NETACP is already serving this node"
                        " (%s names its live request mailbox); a node runs one"
                        " NETACP -- this process is not starting a second\n",
                NETACP_REQ_LOGNAM);
        return 1;
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
    /* rd vms-d3cd: the executive build names the LAN UNIT, never a host
     * interface it would have to enumerate: ETH0: is the executive's own
     * record of the primary NIC, the one _NET: rides. */
    if (!ifname_explicit && decnetd_executive_datalink()) {
        ifname = DECNETD_LAN_UNIT;
    } else if (!ifname_explicit && decnet_autodetect_iface(ifname_auto, sizeof(ifname_auto))) {
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
        /* BARE AUTO-START on an UNCONFIGURED node (cfg->detached: the persistent
         * NETACP daemon SYS$MANAGER:STARTNET.COM launches with no argv, having
         * found no executor address in the node's DECnet configuration). This is
         * NOT an error -- an unconfigured node simply runs no DECnet. Exit CLEAN
         * (success), logging the honest no-op, so STARTNET's RUN/DETACHED leaves
         * neither a failed process nor a %DCL abort on the boot console (INV-6).
         * An EXPLICIT invocation (any flag: --set-host, --show-executor, --router,
         * ...) with no resolvable address is still the caller's error below. */
        if (cfg->detached) {
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
               : decnetd_is_vms_devnam(ifname) ? " (the executive's LAN unit)"
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
    if (cfg->detached)                   /* the detached NETACP STARTNET launched */
        netacp_opcom_starting();
    /* rd vms-d3cd: on the executive build the interface index is the one the
     * executive answered for this open, and the hardware address comes from
     * the LAN unit by $QIO IO$_SENSEMODE (or, for a diagnostic --iface on a
     * host interface name, from the same executive open) -- no host
     * interface lookup. The probe build keeps its host lookups. */
    unsigned ifindex = 0;
    uint8_t hw_mac[6] = {0};
    if (decnetd_executive_datalink()) {
        int xi = scs_datalink_ifindex(sock);
        ifindex = xi > 0 ? (unsigned)xi : 0;
        if (ifindex == 0) {
            fprintf(stderr, "DECNETD-E-NOIFACE, the executive answered no interface"
                            " for '%s'\n", ifname);
            scs_datalink_close(sock);
            return 1;
        }
        if (decnetd_is_vms_devnam(ifname)) {
            uint32_t hst = decnetd_lan_sense_hwaddr(hw_mac);
            if (!(hst & 1)) {
                char why[128] = "";
                (void)vms_status_string(hst, why, sizeof why);
                fprintf(stderr, "DECNETD-E-NOHWADDR, $QIO IO$_SENSEMODE of %s"
                                " failed: %s (status %%X%08X)\n", ifname,
                        why, (unsigned)hst);
                scs_datalink_close(sock);
                return 1;
            }
        } else if (scs_datalink_open_hwaddr(sock, hw_mac) != 0) {
            fprintf(stderr, "DECNETD-E-NOHWADDR, the executive answered no HW"
                            " address for '%s'\n", ifname);
            scs_datalink_close(sock);
            return 1;
        }
    } else {
        ifindex = if_nametoindex(ifname);
        if (ifindex == 0) {
            fprintf(stderr, "DECNETD-E-NOIFACE, unknown interface '%s': %s\n",
                    ifname, strerror(errno));
            scs_datalink_close(sock);
            return 1;
        }
        if (scs_datalink_get_hwaddr(ifname, hw_mac) != 0) {
            fprintf(stderr, "DECNETD-E-NOHWADDR, cannot read HW address of '%s': %s\n",
                    ifname, strerror(errno));
            scs_datalink_close(sock);
            return 1;
        }
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
    g_netacp_circuit_on = 1;     /* the datalink socket is bound (SHOW KNOWN CIRCUITS) */
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
    if (set_host_to) {
        unsigned ta = 0, tn = 0;
        int rrc = sethost_resolve_target(set_host_to, &ta, &tn);
        if (rrc == -2) {
            fprintf(stderr, "DECNETD-E-NODEDB, --set-host: cannot read the node"
                            " database to resolve '%s' (status %08X)\n",
                    set_host_to, (unsigned)dnet_store_last_rms_status());
            scs_datalink_close(sock);
            return 1;
        }
        if (rrc != 0) {
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
     * the executor's MAXIMUM LINKS (rd vms-f91).
     *
     * MEMORY (rd vms-365). A slot is ~70-77 KB (the CTERM host role and the
     * outbound receive queue), so the NETACP_POOL_CAP table is ~4.5-5 MB. It
     * is zero-filled static storage that this process enters once: it was
     * ALREADY zero, and the memset that used to clear it again wrote every
     * page of all 64 slots -- ~4.5 MB made resident for a pool of at most
     * MAXIMUM LINKS (default 32) slots, nearly all of them idle. On a swapless
     * VAX that left the node ~300 KB free and the next fork child died ENOMEM
     * (PRCLOST). Untouched, a slot costs nothing until a link takes it (its
     * `used` word: one page). Only slots below MAXIMUM LINKS are ever
     * reached. */
    static struct netacp_slot slots[NETACP_POOL_CAP];
    /* rd vms-3106: this start's first link address is a fresh per-start
     * point, so a peer still holding a link from before a restart does not
     * mistake our first Connect Initiate for part of it. */
    uint16_t next_lla = dnet_lla_seed(dnet_lla_entropy());
    netacp_vnodes_load();              /* the network starts: volatile <- permanent */
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
                    /* rd vms-acf3: the persistent NETACP (cfg->detached, the
                     * process STARTNET creates) logs no per-hello line --
                     * VMS's NETACP logs no hellos, and one line every hello
                     * timer grew SYS$MANAGER:NETACP.LOG without bound on the
                     * system disk. A diagnostic run (--iface ... from DCL)
                     * still reports each hello it sends. */
                    if (!cfg->detached) {
                        log_ts(stdout);
                        printf(" DECNETD-I-HELLOSENT, %s circuit %s seq=%lu bytes=%zd\n",
                               is_router ? "router-hello" : "endnode-hello",
                               eng.circuit,
                               is_router ? eng.router_hello_sent : eng.hello_sent, sent);
                        fflush(stdout);
                    }
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

/*
 * ===================== the image entry points =====================
 * NETACP.EXE (NETACP_IMAGE): no command line at all. VMS passes a detached
 * process no argv; NCP SET EXECUTOR STATE ON creates it and it sources
 * everything from the executor database and the executive.
 *
 * DECNETD.EXE (DECNETD_SETHOST_IMAGE): ONLY the $ SET HOST client DCL still
 * activates (dcl_cmd_set.c) until RTPAD.EXE replaces it (rd vms-0a7 S2); it
 * is the one argv parser left under src/vmsdecnet and is listed, by name, in
 * the argv gate's baseline (tests/vmsdecnet/argv_gate_baseline.txt), which S2
 * empties.
 *
 * With neither defined, this file is a translation unit a test image includes
 * (tests/vmsdecnet/netacp_test.c): the self-tests live there, not here.
 */
#if defined(NETACP_IMAGE)
int main(void)
{
    struct netacp_cfg c;
    netacp_cfg_default(&c);
    c.detached = 1;
    return netacp_run(&c);
}
#elif defined(DECNETD_SETHOST_IMAGE)
int main(int argc, char **argv)
{
    struct netacp_cfg c;
    netacp_cfg_default(&c);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--set-host") && i + 1 < argc)  c.set_host_to = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc) c.set_host_user = argv[++i];
        else {
            fprintf(stderr, "DECNETD-E-BADARG, unknown argument '%s'\n", argv[i]);
            return 1;
        }
    }
    if (!c.set_host_to) {
        fprintf(stderr, "DECNETD-E-NOTARGET, DECNETD.EXE is the $ SET HOST client;"
                        " NETACP is SYS$SYSTEM:NETACP.EXE (NCP SET EXECUTOR STATE ON)\n");
        return 1;
    }
    return netacp_run(&c);
}
#endif
