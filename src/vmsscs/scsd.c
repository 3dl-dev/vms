/*
 * scsd.c - SCSD skeleton: the SCS Datalink daemon.
 *
 * Per docs/design-cluster-node.md section 3.1: a new src/vmsscs/ subsystem,
 * a userspace daemon that opens a raw-L2 datalink bound to the cluster
 * interface, receives SCA frames (ethertype 0x6007, DEC LAVC/SCA), and
 * classifies them by the GROUNDED length rule (see scs_classify.h /
 * docs/cluster-protocol-spec.md section 2).
 *
 * SCOPE (vms-294): raw socket open + bind + receive loop + length
 * classification + logging + CMake wiring. RECEIVE-ONLY by default.
 *
 * vms-b62 adds the TRANSMIT path: --emit-hello enables a periodic
 * multicast HELLO beacon (scs_hello.c builds the frame; see
 * docs/cluster-protocol-spec.md sec 4a/4b for the wire layout). Still NO
 * NISCA virtual-circuit engine, NO SCS connection multiplexing, NO
 * directed HELLOs, NO connect/membership -- those are later items
 * (vms-5fe). Node identity (SCSNODE) comes from the SYSGEN store
 * (sysgen_read_string, honors OVMX_SYSGEN_PATH); cluster group comes from
 * the cluster-authorize store (cluster_authorize_read, honors
 * OVMX_CLUSTER_AUTH_PATH), defaulting to group 1 (the reference lab's
 * group) if unconfigured.
 *
 * vms-838: the raw-L2 transport is now scs_datalink.h/.c (open/close,
 * MAC-get, send, recv, recv-timeout), not an AF_PACKET socket opened
 * inline in this file. On Linux that backend is byte-for-byte the same
 * AF_PACKET/SOCK_RAW socket this file always opened (still needs
 * CAP_NET_RAW -- run as root, or `setcap cap_net_raw+ep` on the built
 * binary). On NetBSD (the vax substrate) it is bpf(4), which has no
 * AF_PACKET equivalent -- see scs_datalink.h for the split. This file's
 * own socket/ioctl code is gone; every send/recv/MAC-get call site below
 * goes through that header instead, unchanged in shape and behavior.
 */
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>  /* if_nametoindex() -- portable, unlike the raw-socket
                      * headers vms-838 removed (<netpacket/packet.h>,
                      * <sys/socket.h>, <arpa/inet.h>): NetBSD has this one
                      * too. */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "scs_datalink.h"

#include "cluster_authorize.h"
#include "scs_cdt.h"
#include "scs_classify.h"
#include "scs_config.h"
#include "scs_conn.h"
#include "scs_credit.h"
#include "scs_dgram.h"
#include "scs_connect.h"
#include "scs_depart.h"
#include "scs_dir.h"
#include "scs_disc.h"
#include "scs_hello.h"
#include "scs_sdir.h"
#include "scs_member.h"
#include "scs_mscp.h"
#include "scs_mscp_srv.h" /* vms-34b: THE live disk-server responder -- see
                           * scsd_mscp_srv_state() / scsd_mscp_srv_msg_input(). */
#include "scs_poll.h"
#include "scs_quorum.h" /* vms-7a9: CEVOTES/QUORUM computation + quorum gate */
#include "scs_recnx.h"  /* vms-c7d: CSB connectivity states + RECNXINTERVAL reconnect loop */
#include "scs_membership.h"  /* struct scs_cluster_view/member -- vms-967d: struct-only, no file I/O */
#include "scs_reason.h"
#include "scs_env.h" /* vms-ec7: THE shared SCS message envelope (build + parse + dispatch) */
#include "scs_dlm.h" /* vms-94c: the DLM SYSAP message class (ENQ/GRANT/DEQ/BLKAST) */

/* vms-94c: reaching the executive's cross-node DLM handler over /dev/vms. scsd
 * is a glibc process, so it uses a direct POSIX ioctl on /dev/vms (not the
 * freestanding vms_kif client) -- see scsd_dlm_dispatch_to_executive(). */
#ifndef SCSD_UNIT_TEST
#include <sys/ioctl.h>
#include "vms_ioctl.h"
#endif

/* The IOCTL value-block flag (vms_ioctl.h LCK_M_VALBLK == 0x08). The cross-node
 * DLM wire flags field carries the IOCTL flag namespace -- scsd_dlm_dispatch_to_
 * executive copies scs_dlm_msg.flags VERBATIM into vms_dlm_xnode_args.flags, and
 * the executive's cross-node handlers test `req->flags & LCK_M_VALBLK` (NOT the
 * starlet LCK$M_VALBLK == 0x0001). This fallback provides the same value under the
 * SCSD_UNIT_TEST seam, which does not include vms_ioctl.h; the real build's
 * vms_ioctl.h definition is identical (0x08), so this never diverges. */
#ifndef LCK_M_VALBLK
#define LCK_M_VALBLK 0x08
#endif

/* The DLM op vocabulary the executive expects (VMS_DLM_OP_*) MUST match the SCS
 * message opcodes (SCS_DLM_OP_*); scsd is the one place that includes both. */
_Static_assert((int)SCS_DLM_OP_ENQ == 1 && (int)SCS_DLM_OP_GRANT == 2 &&
               (int)SCS_DLM_OP_DEQ == 3 && (int)SCS_DLM_OP_BLKAST == 4 &&
               (int)SCS_DLM_OP_REBUILD == 5 && (int)SCS_DLM_OP_DLKSRCH == 6,
               "SCS_DLM_OP_* must match VMS_DLM_OP_* in vms_ioctl.h");
#include "scs_rx.h"
#include "scs_start.h"
#include "scs_svc.h"
#include "scs_vc.h"
#include "sysgen_params.h"

/* DEC LAVC/SCA ethertype -- docs/cluster-protocol-spec.md sec 2. */
#define SCA_ETHERTYPE 0x6007

/* NISCS_MAX_PKTSZ=1498 (GROUNDED against the SYSGEN tunable, spec sec 2
 * Table 2) plus the 14-byte Ethernet header, rounded up. */
#define SCA_FRAME_MAX 1600

/* Real VAX HELLOs beacon every ~1-2s (observed inter-frame timing in
 * scs-idle-baseline.pcap); default our beacon to the same cadence. */
#define HELLO_DEFAULT_INTERVAL_SEC 2

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

/* Monotonic milliseconds -- feeds the VC retransmit timer (scs_vc_*). */
static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/*
 * scsd_recnxinterval - the local SYSGEN RECNXINTERVAL, in seconds, that sizes
 * the RECNXINTERVAL reconnect period (vms-c7d, transcript p. 7-30). OVMX
 * configuration, NOT a claimed VMS invariant.
 *
 * Precedence:
 *   1. OVMX_RECNXINTERVAL env var  -- explicit test/config override (as before).
 *   2. vms-c3b: the operator-authored SYSGEN RECNXINTERVAL param, read from the
 *      current SYS$SYSTEM:OVMXVMSSYS.PAR through sysgen_read_param() -- the same
 *      store (and the same reader, honoring OVMX_SYSGEN_PATH) scsd already
 *      consults for SCSNODE/SCSSYSTEMID/ALLOCLASS. This is the AUTHORING surface
 *      only: it merely SIZES the reconnect period, exactly as the env override
 *      already did. The reconnect wire/state-machine behavior (scs_recnx.c) is
 *      vms-694's and is untouched.
 *   3. SCS_RECNX_DEFAULT_RECNXINTERVAL (20) -- the fallback when nothing is
 *      authored, matching the SYSGEN param's own factory default (both 20).
 *
 * A value of 0 or an unparseable one at any layer falls through to the next, so
 * the read is a function rather than an inline strtoul -- a test can state what
 * the interval was and where it came from.
 */
static unsigned scsd_recnxinterval(void)
{
    const char *e = getenv("OVMX_RECNXINTERVAL");
    if (e != NULL && *e != '\0') {
        unsigned long v = strtoul(e, NULL, 10);
        if (v > 0UL && v <= 65535UL) {
            return (unsigned)v;
        }
    }
    uint32_t sv = 0;
    if (sysgen_read_param("RECNXINTERVAL", &sv) == 0 && sv > 0u && sv <= 65535u) {
        return (unsigned)sv;
    }
    return SCS_RECNX_DEFAULT_RECNXINTERVAL;
}

/* vms-9f3: OVMX's own local 100ns-tick clock for the HELLO timer field (abs
 * 96-101). The reference-lab wire carries a 48-bit LE counter advancing at
 * ~1e7 ticks/s = 100ns/tick (measured across the af2/ci3 padded directed
 * HELLOs); it is per-sender and the ONLY field that varies across a
 * channel-verify retransmit (spec sec 4k). PEDRIVER rejects a channel whose
 * verify HELLO carries a dead/static timer, so every emitted HELLO stamps the
 * CURRENT tick sampled here. Clean-room: this is OVMX's OWN monotonic clock in
 * 100ns units, never a value copied from VSI source. */
static uint64_t hello_timer_tick100(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 10000000u + (uint64_t)(ts.tv_nsec / 100);
}

/* vms-691: VC keepalive/retransmit tunables. The VAX beacons directed HELLOs
 * on its poller sweep; OVMX re-sends its connect-request if the peer has not
 * acked it within RETRANSMIT_TIMEOUT_MS, capped at RETRANSMIT_MAX to respect
 * the reply-amplification guard (never out-pace real-node cadence). */
#define VC_RETRANSMIT_TIMEOUT_MS 2000u
#define VC_RETRANSMIT_MAX        5u
/* vms-abc: reaching this cap IS the p. 2-31 delivery-guarantee failure, and
 * scs_vc_delivery_failed() is what decides that. The two numbers must be the
 * same one or OVMX would either give up before declaring failure (a silent
 * limp-on, the bug this item removes) or declare failure it never reached. */
_Static_assert(SCS_VC_DELIVERY_RETRY_LIMIT == VC_RETRANSMIT_MAX,
               "scsd's retransmit cap and the p. 2-31 delivery-failure limit disagree");

static void log_ts(FILE *out)
{
    struct timespec ts;
    struct tm tmv;
    char tbuf[16];

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tmv);
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);
    fprintf(out, "[%s.%03ld]", tbuf, ts.tv_nsec / 1000000);
}

/*
 * get_iface_hwaddr - Resolve ifname's hardware (MAC) address (works for
 * both physical NICs and bridge devices like br0).
 *
 * vms-838: delegates to scs_datalink_get_hwaddr() -- SIOCGIFHWADDR on Linux
 * (unchanged), getifaddrs()/AF_LINK on NetBSD (which has no SIOCGIFHWADDR).
 * Kept as a thin named wrapper rather than replacing every call site with
 * scs_datalink_get_hwaddr() directly, so this file's own MAC-resolution
 * call sites (there are several) are untouched.
 */
static int get_iface_hwaddr(const char *ifname, uint8_t mac_out[6])
{
    return scs_datalink_get_hwaddr(ifname, mac_out);
}

/*
 * resolve_node_identity - Read SCSNODE from the SYSGEN store (vms-ci.8;
 * sysgen_read_string honors OVMX_SYSGEN_PATH). Falls back to "OVMX" if
 * NOTHING is configured -- the documented default in
 * docs/design-cluster-node.md sec 6.
 *
 * vms-2f3 2026-08-01 -- BUT NEVER WHEN A STORE WAS EXPLICITLY NAMED.
 * This used to fall back silently in every failure mode, and it cost a
 * session. `oneshot.sh` cd's to the repo root before exec'ing SCSD, so a
 * RELATIVE OVMX_SYSGEN_PATH silently resolved to nothing -- and five lab runs
 * that were supposed to be five distinct fresh identities all went on the wire
 * as the SAME node "OVMX". The "fresh identity" positive controls were
 * therefore rejoins of one identity, the refusals they were meant to validate
 * proved nothing, and a whole hypothesis was refuted on the strength of them.
 * The peers were never confused: they read exactly what we sent.
 *
 * A wrong identity on the wire is the INV-6 silent-fallback bug class that
 * CLAUDE.md rule 9 exists to kill, one layer up from /dev/vms: if the operator
 * pointed us at a store and we could not read it, the honest answer is to die,
 * not to invent a name and advertise it to a live cluster.
 *
 * Returns 0 on success, -1 if a named store could not be read (fatal).
 */
static int resolve_node_identity(char *node_out, size_t node_out_len)
{
    char configured[SYSGEN_STRVAL_LEN];
    if (sysgen_read_string("SCSNODE", configured, sizeof(configured)) == 0 && configured[0] != '\0') {
        strncpy(node_out, configured, node_out_len - 1);
        node_out[node_out_len - 1] = '\0';
        return 0;
    }
    const char *named = getenv("OVMX_SYSGEN_PATH");
    if (named != NULL && named[0] != '\0') {
        fprintf(stderr,
                "SCSD-F-NOIDENT, OVMX_SYSGEN_PATH='%s' names a SYSGEN store but"
                " SCSNODE could not be read from it.\n"
                "                Refusing to fall back to the default node name"
                " -- advertising a node identity the operator did not configure\n"
                "                puts a false name on a live cluster wire."
                " Check the path (it is resolved from the CURRENT WORKING\n"
                "                DIRECTORY, and SCSD may not share yours -- use an"
                " absolute path) and that the store carries SCSNODE.\n",
                named);
        return -1;
    }
    strncpy(node_out, "OVMX", node_out_len - 1);
    node_out[node_out_len - 1] = '\0';
    return 0;
}

/*
 * resolve_cluster_group - Read the cluster group number from the
 * cluster-authorize store (vms-ci.8). Falls back to group 1 -- the only
 * GROUNDED group (the reference lab) -- if unconfigured.
 */
static uint16_t resolve_cluster_group(void)
{
    struct cluster_authorize auth;
    if (cluster_authorize_read(&auth) == 0) {
        return auth.group;
    }
    return SCS_HELLO_MCAST_GROUP1;
}

/* OVMX's default SCSSYSTEMID when the SYSGEN store has none configured. The
 * reference lab uses 1025 (VAX1), 1026 (VAX2), 1027 (VAX3 satellite); 1030 is
 * a non-colliding default so OVMX presents a distinct cluster identity. The
 * live value should come from the vms-ci.8 SYSGEN store (SET SCSSYSTEMID). */
#define OVMX_DEFAULT_SCSSYSTEMID 1030u

/*
 * resolve_scssystemid - Read SCSSYSTEMID from the SYSGEN store (vms-ci.8),
 * falling back to OVMX_DEFAULT_SCSSYSTEMID. Truncated to the 16-bit field the
 * phase-2 START body carries (spec sec 4g [46:48]).
 */
static uint16_t resolve_scssystemid(void)
{
    uint32_t v = 0;
    if (sysgen_read_param("SCSSYSTEMID", &v) == 0 && v != 0) {
        return (uint16_t)(v & 0xffffu);
    }
    /* vms-2f3: same silent-fallback trap as resolve_node_identity -- see the
     * comment there. The identity pair must fail together: VSI OpenVMS Cluster
     * Systems App. A documents that SCSNODE and SCSSYSTEMID may not be changed
     * independently without rebooting the whole cluster, so silently defaulting
     * ONE half while the other is configured is worse than defaulting both.
     * resolve_node_identity() has already made this fatal for a named store, so
     * reaching here with OVMX_SYSGEN_PATH set means SCSNODE read fine and only
     * SCSSYSTEMID is missing -- still an incoherent identity. */
    const char *named = getenv("OVMX_SYSGEN_PATH");
    if (named != NULL && named[0] != '\0') {
        fprintf(stderr,
                "SCSD-W-NOSYSID, OVMX_SYSGEN_PATH='%s' carries SCSNODE but no"
                " usable SCSSYSTEMID; using the default %u.\n"
                "                The identity pair is now half-configured --"
                " peers key their CSBs on BOTH halves.\n",
                named, (unsigned)OVMX_DEFAULT_SCSSYSTEMID);
    }
    return OVMX_DEFAULT_SCSSYSTEMID;
}

/*
 * resolve_alloclass - Read ALLOCLASS from the SYSGEN store (vms-9cf), the
 * allocation class for shared cluster devices. Mirrors resolve_scssystemid()'s
 * sysgen_read_param path and honors OVMX_SYSGEN_PATH the same way.
 *
 * Unlike SCSNODE/SCSSYSTEMID this is NOT half of the fatal identity pair: 0 is
 * the documented default (SYSGEN ALLOCLASS defaults to 0 = "no allocation
 * class", VSI OpenVMS System Manager's Manual). So an absent or 0 value is an
 * honest default, not a mis-config -- no fatal exit, no warning. The value is
 * an IDENTITY/config param the operator authors via SYSGEN/SYSMAN; scsd adopts
 * it on (re)boot the same way it adopts SCSNODE/SCSSYSTEMID. It is read and
 * reported only -- it does NOT touch any wire frame or the VOTES=0 non-voting
 * join (that reconciliation is vms-41d, owned by the cluster-wire session).
 *
 * Returns the authored allocation class, or 0 when unconfigured.
 */
static uint8_t resolve_alloclass(void)
{
    uint32_t v = 0;
    if (sysgen_read_param("ALLOCLASS", &v) == 0) {
        return (uint8_t)(v & 0xffu);
    }
    return 0u;
}

/*
 * ovmx_cluster_logical - vms-9f3: compute OVMX's cluster-LOGICAL LAVC address
 * from its SCSSYSTEMID. GROUNDED convention (spec sec 3 decoder ring + README-lab):
 * a cluster node's logical addr is aa:00:04:00:<LE16(SCSSYSTEMID)>, e.g. VAX2
 * sysid 1026=0x0402 -> aa:00:04:00:02:04. This is the value a real member writes
 * in the SCA src-logical field (abs 24); OVMX's raw HW MAC there was why VAX1's
 * PEDRIVER never verified the channel (the old bug). No DECnet is needed to emit
 * it -- it is a cluster field that happens to use the LAVC address format.
 */
static void ovmx_cluster_logical(uint16_t sysid, uint8_t out[6])
{
    out[0] = 0xaa;
    out[1] = 0x00;
    out[2] = 0x04;
    out[3] = 0x00;
    out[4] = (uint8_t)(sysid & 0xff);        /* LE16 low byte  */
    out[5] = (uint8_t)((sysid >> 8) & 0xff); /* LE16 high byte */
}

/* --- vms-5fe: directed-HELLO / SCS-connect responder state --- */

#define OVMX_MAX_PEERS 4

/* vms-298 / vms-584 item 5: THE CON.ID HIGH WORD IS PER-BOOT, NOT COMPILE-TIME.
 *
 * Every OVMX Con.ID used to be a compile-time constant: SCS_CONNECT_OVMX_CONID_
 * BASE (0x4F58) in the high word, a fixed slot index in the low word. That makes
 * OVMX the only node on the wire whose connection identifiers are IDENTICAL on
 * every boot.
 *
 * GROUNDED (capture census, vms-584): a real node's Con.IDs are a single
 * monotonic counter shared across ALL service classes within one boot --
 * formation-ci1, node 08:00:2b:78:56:b9: SCS$DIRECTORY 0x33590007,
 * VMS$VAXcluster 0x33580008, MSCP$DISK 0x33580009 -- and the HIGH word RESEEDS
 * non-arithmetically at each incarnation of the same node identity:
 * af2-firsttimer shows 0x8fd20007 -> 0xe9950007 -> 0x5b050007 for the same
 * class across three boots. A real node's Con.ID for a given class therefore
 * NEVER repeats across incarnations.
 *
 * ALSO GROUNDED, and the reason this is safe to change at all: the peer binds
 * whatever value is offered and never validates it. 30+ CONNECT sequences in
 * formation-ci1 carry unpredictable values and every one is answered
 * ECHO -> ACCEPT -> CONFIRM; no DISC-RSP is ever substituted, and no NAK in the
 * library is tied to a Con.ID value.
 *
 * WHY IT MATTERS NOW: the join/exit cycling test (vms-584 item 5) is exactly
 * the untested collision case -- OVMX leaves and rejoins under the same
 * SCSNODE/SCSSYSTEMID, and with compile-time constants it would offer the peer
 * a Con.ID identical to the one it had in the previous incarnation. No capture
 * exercises that, because a real allocator can never produce it.
 *
 * WHAT IS OVMX'S OWN DESIGN CHOICE, LABELLED AS SUCH (Rule 8): the DERIVATION of
 * the high word. The reference's reseed looks like an address or a clock, but
 * nothing in the public documentation publishes it and no wire evidence pins it,
 * so OVMX picks its own per-boot value and does not present it as VMS-authentic.
 * The low-word slot indices below are unchanged -- they are already distinct
 * within a boot, which is the property the reference's shared counter provides.
 * OVMX_CONID_BASE may be pinned via the environment for a reproducible run.
 */
static uint32_t g_ovmx_conid_base;   /* 0 until first use; high word, <<16 */

/*
 * vms-578: THE MSCP$DISK SERVER SWITCH -- ONE FLAG FOR ONE PRODUCT DECISION.
 *
 * The two merged branches disagree about whether OVMX is an MSCP$DISK server,
 * and the disagreement is real, not cosmetic:
 *
 *   worktree-760 ACCEPTS the member-opened MSCP$DISK connect (op=1 echo, op=4
 *   accept) and AFFIRMS MSCP$DISK in its SCS$DIR_LOOKUP answers, having
 *   measured that the established member does not open that connect without
 *   the HIT -- a step on the path from NEW to MEMBER.
 *
 *   work/vms-187-closure REFUSES it. vms-7fe made p. 2-22's "list of listening
 *   SYSAPs" the single source of truth for both the lookup answer and the
 *   p. 2-48 connect scan, and deliberately left MSCP$DISK out because OVMX
 *   implements no MSCP disk-server COMMAND handling (scs_mscp.c is a CLIENT).
 *   Its standing gates assert that refusal by name:
 *   test_sdir_lookup_is_answered_from_the_queue and
 *   test_sdir_refuses_a_connect_request_for_an_unlisted_sysap.
 *
 * BOTH WERE DEFENSIBLE, AND THE RULING CAME IN (vms-34b, operator, 2026-08-06):
 * BUILD THE FULL SERVER. "Advertising a SYSAP whose commands OVMX cannot
 * serve is the INV-6 shape" is exactly why this flag stayed OFF by default
 * for as long as that was true -- but it stopped being true the moment
 * scsd_mscp_srv_msg_input() below started existing. vms-291 (PR #131) built
 * the responder (SCC/GUS/ONLINE/READ/WRITE end messages, byte-exact against a
 * real captured VAX server) and vms-4e31 (PR #142) un-deferred vms-941's block
 * data transfer -- but NEITHER wired the responder to the live daemon: no CDT
 * was ever allocated at OVMX_MSCP_SERVER_CONID, so every command a real class
 * driver sent after the op=4 accept was silently dropped
 * (SCS_DELIVER_NO_CDT), which is a WORSE facade than refusing the connect
 * outright -- "silence is never a response" (scs_mscp_srv.h). vms-34b closes
 * that: the accept path below now allocates the CDT and installs
 * scsd_mscp_srv_msg_input(), so every command gets a real, honest end message
 * (Unit-Offline / Invalid Command / Write Protected when nothing is attached,
 * a real answer once an operator attaches a unit) instead of vanishing. THAT
 * is what makes flipping this flag's default honest rather than a bigger lie.
 *
 * DEFAULT ON. The flag drives EVERYTHING -- the LISTEN registration, the
 * lookup affirmative and the inbound connect accept -- so the queue can never
 * disagree with the wire. OVMX_MSCP_SERVER=0 is the kill switch, preserved
 * because a wire-visible default flip ships one (project convention). Setting
 * it to exactly "1" (the old opt-in spelling) is unaffected: still on.
 *
 * GUARDRAIL 23 -- THE SWITCH WAS RUN BOTH WAYS BEFORE THIS DEFAULT FLIPPED.
 * lab-2 pod vaxlab-4, 2026-08-05, arms B2 (off) and B4 (on): SDIR listening
 * 2 -> 3, MSCP-SERVER-ACCEPTS-SENT 0 -> 4, SCSD-I-MSCPSRV lines 0 -> 10. All
 * three gated behaviours move together, which is the point of putting them on
 * one flag. BOTH ARMS JOINED (CLUSTER_NODES=3, XITDONE=1) -- so the MSCP$DISK
 * affirmative was not, on that lab, what decided a first join
 * (docs/cluster-protocol-spec.md sec 4(O.1)). That measurement is why this
 * default flip is not expected to change join behaviour; it has not been
 * re-run against the CDT wiring added here, which is why the kill switch
 * stays available rather than being deleted.
 */
static int ovmx_mscp_server_enabled(void)
{
    const char *env = getenv("OVMX_MSCP_SERVER");
    return !(env != NULL && env[0] == '0' && env[1] == '\0');
}

static uint32_t ovmx_conid_base(void)
{
    if (g_ovmx_conid_base == 0u) {
        const char *env = getenv("OVMX_CONID_BASE");
        if (env != NULL && *env != '\0') {
            g_ovmx_conid_base = (uint32_t)(strtoul(env, NULL, 0) & 0xFFFFu) << 16;
        }
#ifdef SCSD_UNIT_TEST
        /* vms-578: THE UNIT-TEST SEAM PINS THE BASE, AND SAYS WHY.
         *
         * The randomization below is a PRODUCTION behaviour: its whole point is
         * that two OVMX incarnations issue different Con.IDs (vms-2f3 -- a
         * returning node must not present its previous incarnation's handles).
         * tests/vmsscs/test_scsd_wire.c replays CAPTURED frames, which carry the
         * Con.IDs of the run that produced them (0x4F58....), so a randomized
         * base makes every captured-frame fixture miss its CDT. Pinned here to
         * the historical base so the captures stay addressable.
         *
         * STATED PLAINLY: this means the randomization ITSELF is not exercised
         * by the wire tests -- they run with it pinned. It is still reachable
         * from a test through OVMX_CONID_BASE, which is checked first and
         * overrides this. */
        if (g_ovmx_conid_base == 0u) {
            g_ovmx_conid_base = SCS_CONNECT_OVMX_CONID_BASE;
        }
#endif
        if (g_ovmx_conid_base == 0u) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            /* Mix the clock with the pid so two OVMX incarnations started in
             * the same second still differ -- the cycling test restarts SCSD
             * within seconds, which is precisely when a coarse clock collides. */
            uint32_t h = (uint32_t)ts.tv_nsec ^ ((uint32_t)ts.tv_sec << 11) ^
                         ((uint32_t)getpid() << 3);
            h &= 0xFFFFu;
            if (h == 0u) h = 0x4F58u;   /* never zero -- 0 reads as "no handle" */
            g_ovmx_conid_base = h << 16;
        }
    }
    return g_ovmx_conid_base;
}

/* vms-2f3: THIS SYSTEM'S INCARNATION IS A LIVE TIMESTAMP, NOT A REPLAYED ONE.
 *
 * The START/config (0x41) body carries a VMS absolute-time quadword at [66:74]
 * that a peer stores as our "Incarnation" -- SDA on the real VAX1 renders it in
 * OVMX's CSB, and every real peer's CSB carries that node's BOOT time. Ours
 * shipped straight out of the captured template on every boot
 * (0x00bc00947a678ebb = 26-JUL-2026 14:35:33.59), so OVMX was the only node on
 * the wire whose incarnation never changed.
 *
 * That is what blocked the rejoin. A peer holding a crash-removed OVMX's CSB in
 * state wait/long_break, seeing a returning OVMX advertise the IDENTICAL
 * incarnation, has no reason to believe the node rebooted -- so it hands back
 * the SAME System Block (observed: SB address 87A0C580 unchanged across the
 * dead-CSB dump and the rejoin attempt) carrying the previous incarnation's VC
 * sequence state, while OVMX has reset its own VC to send_seq=1. The peers open
 * a transition for us, never get an ack on that circuit, and abandon it:
 * "timed-out lost connection to node OVMXC2" on all three.
 *
 * ONE VALUE PER PROCESS, sampled once: this is our BOOT time, not "now". Every
 * frame of one OVMX run must carry the same incarnation, or each START would
 * look like a different system.
 *
 * OVMX DESIGN CHOICE, LABELLED (Rule 8): that OVMX's "boot" is its process
 * start. The FORMAT (VMS 100 ns since 17-NOV-1858) is public and the field's
 * ROLE is grounded on the wire + in SDA output; nothing about how a real VAX
 * derives its own value is copied, because nothing needs to be.
 *
 * OVMX_INCARNATION_TIME=<n>  pins the quadword for a reproducible run.
 * OVMX_INCARNATION_FROZEN=1  restores the replayed template bytes -- the
 *                            control arm of the vms-2f3 experiment ONLY.
 */
static uint64_t g_ovmx_incarnation_time;   /* 0 until first use */

static uint64_t ovmx_incarnation_time(void)
{
    if (g_ovmx_incarnation_time == 0u) {
        const char *frozen = getenv("OVMX_INCARNATION_FROZEN");
        if (frozen != NULL && *frozen == '1') {
            return 0u;  /* leave the template bytes; never cached */
        }
        const char *env = getenv("OVMX_INCARNATION_TIME");
        if (env != NULL && *env != '\0') {
            g_ovmx_incarnation_time = (uint64_t)strtoull(env, NULL, 0);
        }
        if (g_ovmx_incarnation_time == 0u) {
            /* Same epoch conversion as scs_member_vms_time_now(): the POSIX
             * epoch is 3506716800 s after 17-NOV-1858. Nanosecond resolution so
             * two OVMX incarnations started inside the same second still
             * differ -- the cycling test restarts SCSD within seconds, which is
             * exactly when a whole-second clock collides. */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            g_ovmx_incarnation_time =
                ((uint64_t)ts.tv_sec + 3506716800ULL) * 10000000ULL +
                (uint64_t)(ts.tv_nsec / 100);
        }
    }
    return g_ovmx_incarnation_time;
}

/* OVMX's own VMS$VAXcluster Con.ID for this run (OVMX design choice; opaque
 * to the peer -- see scs_connect.h). We use two distinct handles: one for the
 * connection the MEMBER opens to OVMX (answered with a CONNECT-RESPONSE), and a
 * separate one for the connection OVMX opens to the member as an ACTIVE JOINER
 * (the clean-ref grounded requirement, vms-d94: VAXB opens its OWN
 * VMS$VAXcluster connection and sends the add-member burst on IT --
 * formation-clean-2node.pcap idx52/59). A single handle for both directions
 * would collide the two SCS connections. */
#define OVMX_LOCAL_CONID  (ovmx_conid_base() | 0x0001u)
#define OVMX_JOINER_CONID (ovmx_conid_base() | 0x0002u)
/* vms-760: OVMX's MSCP$DISK client connection handle (VMS$DISK_CL_DRVR ->
 * MSCP$DISK). Distinct from 0x0001 (local), 0x0002 (joiner VC), 0x0007 (dir),
 * 0x0008 (own dir). */
#define OVMX_MSCP_CONID   (ovmx_conid_base() | 0x000Au)
/* vms-760 SERVER-FIRST established-join: OVMX's MSCP$DISK SERVER connection
 * handle -- the fresh local Con.ID OVMX supplies when it ACCEPTS the MEMBER's
 * inbound MSCP$DISK connect (op=4 accept). Distinct from every other handle:
 * 0x0001 VC(local)/0x0002 VC(joiner)/0x0007 dir-server/0x0008 dir-client/
 * 0x000A MSCP-client. Opaque to the peer (OVMX design choice, scs_connect.h). */
#define OVMX_MSCP_SERVER_CONID (ovmx_conid_base() | 0x000Bu)

/* vms-34b: THE LIVE MSCP DISK-SERVER RESPONDER's controller identity.
 *
 * P.CNTI (sec 6.16 controller identifier): OVMX DESIGN CHOICE. scs_mscp_srv.h
 * records that a real controller's P.CNTI carries its own node logical-address
 * suffix in the low word -- an OBSERVATION about shape, not a spec requirement
 * (sec 6.16 does not publish a required layout, only "a unique identifier for
 * this controller"). Copying a captured VAX/HSC's own value here would claim
 * OVMX's controller IS that specific real device, which Rule 8 forbids; this
 * value is OVMX's own and stable across restarts.
 * P.CTMO (sec 6.16 controller timeout, seconds): 20, matching the peer-timeout
 * default OVMX already uses elsewhere (scs_depart.h
 * SCS_DEPART_LISTEN_TIMEOUT_DEFAULT_MS / 1000) rather than a second, unrelated
 * invented number. */
#define OVMX_MSCP_SRV_CTLR_ID      ((uint64_t)0x4F564D5800000001ULL) /* "OVMX" + 1 */
#define OVMX_MSCP_SRV_CTLR_TIMEOUT 20u

/* The SCC end message's two GROUNDED-BUT-UNEXPLAINED fields
 * (docs/design-mscp-direction.md Phase D, scs_mscp_srv.h): every SET
 * CONTROLLER CHARACTERISTICS end message in the lab corpus (954/954) carries
 * P.CNTF=0xa004 and the Table A-7 "reserved" word at [18:20]=0x0547, values no
 * public spec explains but every real VMS 7.3 server sends. Passed to
 * scs_mscp_srv_set_ctlr_profile() so OVMX's own SCC end message is not
 * distinguishable from a real controller's on the wire -- that is what the
 * function exists for. */
#define OVMX_MSCP_SRV_CTLR_FLAGS   0xa004u
#define OVMX_MSCP_SRV_CTLR_VERSION 0x0547u

/* vms-600: P.MEDI for a served unit. docs/cluster-protocol-spec.md sec on
 * P.MEDI (the GUS-end-message field table) decodes this GROUNDED value --
 * AA-L619A-TK sec 4.17 + Appendix C's worked example, calibrated against the
 * lab's own RQ0/RQ1 config -- as DU RA92: `0x2564105c`. OVMX is not RA92
 * hardware and a served unit here is a FILE, so this is a labeled DESIGN
 * CHOICE riding on a real decode, not a hardware claim: geometry
 * (track/group/cylinder/RCT) is left at scs_mscp_srv_attach_fd()'s spec
 * default of 0 ("0 if inapplicable", sec 6.12) rather than asserting RA92
 * geometry for a volume whose real block count is whatever file is attached. */
#define OVMX_MSCP_SRV_MEDIA_ID 0x2564105cu

/* vms-760 PURE-SERVER disk-CLIENT (established-join): after OVMX reaches NEW as a
 * pure server, a real joiner is ALSO a disk CLIENT -- it opens its OWN
 * SCS$DIRECTORY + MSCP$DISK connections BACK to VAX1 and runs MSCP disk discovery
 * (SET CONTROLLER CHARACTERISTICS + GET UNIT STATUS enumeration), proving it can
 * reach the cluster system disk before VAX1 commits it to MEMBER (af2-firsttimer-
 * established). These two handles are DISTINCT from every other conid -- in
 * particular from the OVMX_JOIN_SEQ active-joiner handles (SCS_DIR_OVMX_JOINER_
 * CONID 0x0008 / OVMX_MSCP_CONID 0x000A) -- so this pure-server client path is
 * fully isolated: the existing join-sequencer receive handlers key on those other
 * conids and never match these, and vice-versa (no cross-drive, Rule-9 clean). */
#define OVMX_PS_DIR_CONID  (ovmx_conid_base() | 0x000Cu)
#define OVMX_PS_MSCP_CONID (ovmx_conid_base() | 0x000Du)

/* scs_dir.h defines the two SCS$DIRECTORY handles against the COMPILE-TIME base,
 * because the encoders take a Con.ID as a parameter and the unit tests need a
 * fixed value to assert an echo against. The DAEMON must offer the per-boot base
 * like every other handle, or these two would be the only Con.IDs OVMX repeats
 * across incarnations -- which is the whole defect above. Override them here,
 * keeping the low-word slot indices exactly as the header documents them, so
 * every comparison site in this file continues to compare like with like. */
#undef  SCS_DIR_OVMX_CONID
#undef  SCS_DIR_OVMX_JOINER_CONID
#define SCS_DIR_OVMX_CONID        (ovmx_conid_base() | 0x0007u)
#define SCS_DIR_OVMX_JOINER_CONID (ovmx_conid_base() | 0x0008u)

/*
 * vms-694 / vms-584: PER-PEER Con.IDs -- the fix for the established-multi-peer
 * NEW->MEMBER stall (docs/cluster-protocol-spec.md sec 4(t)/4(x)).
 *
 * Every macro above puts a FIXED low word on the wire for a given connection
 * class -- 0x0001 local VC, 0x0002 joiner VC, 0x0007/0x0008 directory, 0x000A
 * MSCP client, 0x000B MSCP server, 0x000C/0x000D pure-server. Fixed means
 * NODE-GLOBAL: OVMX offered the identical Con.ID to EVERY peer, so the moment a
 * SECOND established member answered, its reply resolved to the SAME CDL slot
 * (the low 16 bits index the CDL, scs_cdt.h) that the first peer's connection
 * had already bound. scs_cdl_resolve() then dropped it as SCS_DELIVER_SRC_
 * MISMATCH (SCSD-W-RXSRCMISMATCH): the second peer's legitimate CM traffic was
 * silently discarded and the join to a running 2+-member cluster never
 * advanced past status=NEW. Reproduced live 3x on lab-2 (vaxlab-0). This is the
 * fixed-vs-per-peer hazard scsd_svc_slot_refused() and the vms-73c/vms-298
 * comments (MSCP-server path below) had already flagged as "NOT FIXED HERE."
 *
 * THE FIX: each concurrently-live peer gets its OWN 0x10-wide block of low
 * words. peer slot i uses (i * OVMX_PEER_CONID_STRIDE) | <class>, so the two
 * nodes' connections land in DISTINCT CDL slots and each peer's echo resolves
 * to its own CDT. peer slot 0 keeps offset 0 -- i.e. the EXACT historical
 * class-slot values -- so every single-peer capture and the 19-frame replay
 * library replay byte-identically; only the 2nd+ peer moves.
 *
 * Rule 8 / clean-room: the wire framing/echo/match rules are UNCHANGED. A
 * Con.ID is an opaque handle the peer echoes and never validates (grounded,
 * vms-584: 30+ CONNECT sequences carry unpredictable values, all answered). The
 * VALUE allocation is local policy; the block offset is OVMX's own design
 * choice, opaque to the peer exactly as the per-boot high word is, and never
 * presented as VMS-authentic. VMS's own allocator is a single monotonic counter
 * shared across classes (grounded, lines above); OVMX keeps its fixed class
 * slots + a per-peer block, which is a labeled OVMX derivation, not a claim of
 * VMS-authenticity.
 */
#define OVMX_PEER_CONID_STRIDE 0x10u
/* Connection-class low nibbles (must each be < OVMX_PEER_CONID_STRIDE and
 * distinct, so class = low & (STRIDE-1) and peer index = low / STRIDE). */
#define OVMX_CONID_CLS_LOCAL   0x0001u  /* member-opened VMS$VAXcluster VC */
#define OVMX_CONID_CLS_JOINER  0x0002u  /* OVMX-opened VMS$VAXcluster VC */
#define OVMX_CONID_CLS_DIR     0x0007u  /* SCS$DIRECTORY server (member-opened) */
#define OVMX_CONID_CLS_DIRJ    0x0008u  /* SCS$DIRECTORY client (OVMX-opened) */
#define OVMX_CONID_CLS_DIRPOLL 0x0009u  /* SCS$DIR_LOOKUP poller (OVMX-opened) */
#define OVMX_CONID_CLS_MSCP    0x000Au  /* MSCP$DISK client (OVMX-opened) */
#define OVMX_CONID_CLS_MSCPSRV 0x000Bu  /* MSCP$DISK server (member-opened) */
#define OVMX_CONID_CLS_PSDIR   0x000Cu  /* pure-server SCS$DIRECTORY client */
#define OVMX_CONID_CLS_PSMSCP  0x000Du  /* pure-server MSCP$DISK client */
#define OVMX_CONID_CLS_DLM     0x000Eu  /* DLM SYSAP client (OVMX-opened)  -- vms-94c */
#define OVMX_CONID_CLS_DLMSRV  0x000Fu  /* DLM SYSAP server (peer-opened)  -- vms-94c */

/* OVMX's own handle of class `cls` for peer `ps` -- the per-peer replacement
 * for the fixed OVMX_*_CONID macros. `ps` must be non-NULL. */
#define PS_CONID(ps, cls) \
    (ovmx_conid_base() | (uint32_t)(ps)->conid_off | (uint32_t)(cls))
#define PS_LOCAL_CONID(ps)          PS_CONID((ps), OVMX_CONID_CLS_LOCAL)
#define PS_JOINER_CONID(ps)         PS_CONID((ps), OVMX_CONID_CLS_JOINER)
#define PS_SCS_DIR_CONID(ps)        PS_CONID((ps), OVMX_CONID_CLS_DIR)
#define PS_SCS_DIR_JOINER_CONID(ps) PS_CONID((ps), OVMX_CONID_CLS_DIRJ)
/* vms-694: the poller handle (scs_dir.h's SCS_DIR_OVMX_POLL_CONID, class 0x0009)
 * is folded in here too. scs_dir.h defines it against the COMPILE-TIME base
 * because the vms-760 #undef that moved every other directory handle onto the
 * per-boot ovmx_conid_base() missed it -- so it was the one OVMX handle that did
 * NOT reseed per incarnation (the exact defect that #undef fixed for its
 * siblings). Routing it through PS_CONID both makes it per-peer AND puts it on
 * the per-boot base like every other handle. */
#define PS_SCS_DIR_POLL_CONID(ps)   PS_CONID((ps), OVMX_CONID_CLS_DIRPOLL)
#define PS_MSCP_CONID(ps)           PS_CONID((ps), OVMX_CONID_CLS_MSCP)
#define PS_MSCP_SERVER_CONID(ps)    PS_CONID((ps), OVMX_CONID_CLS_MSCPSRV)
#define PS_PS_DIR_CONID(ps)         PS_CONID((ps), OVMX_CONID_CLS_PSDIR)
#define PS_PS_MSCP_CONID(ps)        PS_CONID((ps), OVMX_CONID_CLS_PSMSCP)
/* vms-94c: the DLM SYSAP handles. Client = the handle OVMX opens toward a peer's
 * DLM SYSAP; server = the handle a peer's DLM SYSAP connects TO on this node
 * (where a received cross-node DLM request lands). Per-peer like every handle. */
#define PS_DLM_CONID(ps)            PS_CONID((ps), OVMX_CONID_CLS_DLM)
#define PS_DLM_SERVER_CONID(ps)     PS_CONID((ps), OVMX_CONID_CLS_DLMSRV)

/*
 * Does `conid` name one of OVMX's OWN per-peer handles of connection class
 * `cls` (any peer slot)? This replaces the receive-side `rconid == OVMX_*_CONID`
 * equality tests: the frame is still routed to its peer by src MAC
 * (peer_find_or_add), so this only has to confirm the ECHOED handle is our
 * handle FOR THAT CLASS -- for peer slot 0 it matches exactly the historical
 * constant, and it now ALSO matches slots 1..N-1 (the values the fix put on the
 * wire), which the old equality test dropped. The high word must be our base
 * and the peer-index field must be in range, so a stray/foreign Con.ID that
 * merely shares a class nibble does not match.
 */
static inline int ovmx_conid_is_class(uint32_t conid, uint16_t cls)
{
    if ((conid & 0xFFFF0000u) != ovmx_conid_base()) {
        return 0;
    }
    unsigned low = (unsigned)(conid & 0xFFFFu);
    if ((low & (OVMX_PEER_CONID_STRIDE - 1u)) != (unsigned)cls) {
        return 0;
    }
    return (low / OVMX_PEER_CONID_STRIDE) < (unsigned)OVMX_MAX_PEERS;
}

/* vms-760 pure-server disk-client state machine (stop-and-wait; one drive frame
 * outstanding). Advanced receive-driven by VAX1's echoes on OUR PS conids. NEVER
 * opens a VMS$VAXcluster VC (that would short-circuit admission -- the whole
 * point of pure-server); only the dir + MSCP CLIENT connects + MSCP discovery. */
enum ps_client_step {
    PSC_IDLE = 0,
    PSC_DIR_CONNECT,     /* sent OUR dir connect; await VAX1 op2 accept */
    PSC_DIR_LOOKUP_TAPE, /* sent dir confirm + MSCP$TAPE lookup; await response (miss) */
    PSC_DIR_LOOKUP_DISK, /* sent MSCP$DISK lookup; await HIT */
    PSC_MSCP_CONNECT,    /* sent OUR MSCP$DISK connect; await VAX1 op2 accept */
    PSC_SCC,             /* sent MSCP confirm + SET CONTROLLER CHARACTERISTICS; await SCC-END */
    PSC_GUS,             /* GET UNIT STATUS enumeration in progress */
    PSC_DONE             /* enumeration hit the OFFLINE terminator -- disk discovery complete */
};

/* Absolute frame offsets used by the responder (spec byte-offset convention:
 * 0 = first byte of Ethernet dst). */
#define OFF_ETH_DST      0
#define OFF_ETH_SRC      6
#define OFF_HELLO_SRCLOG 24  /* HELLO SCA src-logical addr (abs 24-29) */
#define OFF_HELLO_DIRFLG 92  /* directed-HELLO flag (abs 92-93) */

/*
 * struct peer_state - vms-7be: what used to be a MAC-keyed struct describing a
 * remote node is now a THIN INDEX over the architected SCA structures.
 *
 * The node and the virtual circuit are described by the Path Block and System
 * Block that `pb` points at (VAXcluster Principles ch. 2, pp. 2-11..2-21, see
 * scs_config.h): the peer's Ethernet port address lives in pb->remote_port_addr
 * (p. 2-12), its 48-bit SCS System Address in pb->sb->system_id (p. 2-16), and
 * the state of the virtual circuit in pb->vc_state (p. 2-11) -- all of which
 * previously lived here as eth_mac/logical/ad-hoc flags.
 *
 * What REMAINS here is deliberately not SB/PB material: it is SCS CONNECTION
 * and SYSAP state (Con.ID pairs, directory handles, the connection-manager
 * dialogue) plus NISCA channel bookkeeping. Connections ride ON a virtual
 * circuit and are a layer above it (p. 2-11 Figure 2-6); moving them into
 * connection descriptors is a separate item.
 */

/* vms-760: NEW->MEMBER join sequencer states (stop-and-wait; one frame
 * outstanding). Each WAIT state is advanced by the matching member frame in the
 * receive path. See docs/design-cluster-join-choreography.md. */
enum join_step {
    JS_IDLE = 0,
    JS_DIR_CONNECT,      /* sent own dir connect-req; await member echo(op1)+response(op2) */
    JS_DIR_LOOKUP_TAPE,  /* sent confirm + MSCP$TAPE lookup; await member response (miss) */
    JS_DIR_LOOKUP_DISK,  /* sent MSCP$DISK lookup; await member HIT */
    JS_MSCP_CONNECT,     /* sent MSCP$DISK connect; await member accept (MSCPBOUND) */
    JS_DIR_LOOKUP_VC,    /* sent VMS$VAXcluster lookup; await member HIT */
    JS_VC_CONNECT,       /* sent VC connect; await member accept (JOINBOUND) */
    JS_ADD_MEMBER,       /* sent add-member burst; await member reciprocation -> MEMBER */
    JS_DONE
};
#define JOIN_RETX_TIMEOUT_MS 400u  /* stop-and-wait step retransmit (< member's ~1.4s START-reissue) */
/* vms-694 (2026-08-09): 6 is BELOW the reference's own grounded requirement.
 * A live lab-2 rejoin (vms-600 #209's vaxlab-0 capture, vms600-scsd.log)
 * reproduced the vms-2f3 stall directly: the peer answered OUR MSCP$DISK
 * CONNECT_REQ with an op-4 REJECT_REQ on the REJOIN attempt (fresh joins to
 * the same pod moments earlier had bound cleanly), OVMX's join_step stuck at
 * JS_MSCP_CONNECT and retransmitted CONNECT_REQ, and the run's own duration
 * timer closed the VC after only 6 retransmits with the peer still refusing.
 * docs/cluster-protocol-spec.md sec (1h) already documents WHY the real
 * protocol needs more attempts than that: two real VAXes in
 * af2-firsttimer-established-20260728.pcap run "NINE consecutive 4/5
 * exchanges ... at ~10 s intervals ... and a TENTH attempt switches message
 * type entirely -- to 2/3 -- and succeeds". A cap of 6 gives up three
 * attempts short of where the reference itself first succeeds, so OVMX was
 * never going to get in on a rejoin no matter how long the caller waited.
 * 12 clears the grounded floor of 9 rejects-then-accept with margin for wire
 * jitter; it does not change what OVMX puts on the wire, only how many times
 * it retries the SAME CONNECT_REQ before conceding. See
 * tests/vmsscs/test_scsd_wire.c's nine-reject regression. */
#define JOIN_RETX_MAX        12u
/* vms-694 (2026-08-09): the MSCP$DISK CONNECT-REQUEST retransmit CADENCE --
 * grounded SEPARATELY from the generic JOIN_RETX_TIMEOUT_MS above, not folded
 * into it, because JOIN_RETX_TIMEOUT_MS ALSO drives JS_DIR_CONNECT,
 * JS_DIR_LOOKUP_{TAPE,DISK,VC} and JS_VC_CONNECT (plus PSC_DIR_CONNECT /
 * PSC_DIR_LOOKUP_* under OVMX_PURE_SERVER), none of which this measurement
 * covers -- widening it would be an UNGROUNDED change to steps nobody has
 * measured. Two sources, in agreement:
 *   (1) docs/cluster-protocol-spec.md sec (1h): "a different Con.ID family
 *       runs NINE consecutive 4/5 exchanges between the same two nodes at
 *       ~10 s intervals with a STRICTLY INCREASING Con.ID ... and a TENTH
 *       attempt switches message type entirely -- to 2/3 -- and succeeds".
 *   (2) Direct re-measurement off the SAME capture
 *       (af2-firsttimer-established-20260728.pcap), real-VAX-source (OUI
 *       08:00:2b) 62-byte msgtype-4/5 frames only, sorted by wire timestamp:
 *       28 frames, 27 inter-frame deltas, 25 "steady-state" deltas (the other
 *       2 are >100s idle gaps between separate reject-storm runs, not this
 *       cadence) span 9.489s-10.434s, mean 10.023s, and the great majority
 *       cluster tightly at 9.99-10.02s -- the three ~10.2-10.4s outliers are
 *       each the FIRST retry after one of those idle gaps, not the steady
 *       cadence. 10000ms is the grounded value both sources converge on.
 * OVMX's prior 400ms (JOIN_RETX_TIMEOUT_MS, shared with every other step) was
 * 25x too fast against this reference -- retrying at 25x the real cadence
 * risks a real peer treating OVMX as misbehaving mid-reject-storm, which is
 * exactly the storm JOIN_RETX_MAX=12 (above) exists to survive. Override for
 * LAB EXPERIMENTS ONLY, never for production cadence: OVMX_MSCP_CONNECT_
 * RETX_MS (see mscp_connect_retx_timeout_ms()). */
#define MSCP_CONNECT_RETX_TIMEOUT_MS 10000u
/* vms-e81: a NEWCOMER gets a far longer offer window than a settled peer.
 * Run by5: OVMX correctly opened its SCS$DIRECTORY to VAX3, VAX3 never
 * answered, and OVMX gave up after 6 retries / ~12 s -- WHILE VAX3 WAS STILL
 * JOINING. A node in the middle of its own admission may simply not be
 * accepting foreign connects yet, in which case the answer is patience, not
 * protocol. 60 retries at 3 s covers a full VAX boot+join (~2-4 min). If it
 * STILL refuses after this, the cause is membership propagation, not timing. */
#define JOIN_GREET_RETX_MAX  60u
/* vms-760: delay before the joiner's DEFERRED op 0x02 config/topology message.
 * The 2->3 reference joiner sends MODEL+PARAMS at +0.9394 and op 0x02 at
 * +5.8774 -- ~4.9 s later. What it is really waiting on is not grounded; the
 * delay is copied from the reference. */
#define JOIN_CFG2_DELAY_MS   4900u
/* vms-760: how long to hold the step-1 barrier request after the op 0x0a GO.
 * The coordinator's measured per-member fan-out gap is 20-214 us; 3 ms clears it
 * with a wide margin while staying far inside the transition's own timeout (the
 * reference holds step 5 for 89 ms without complaint). Tunable for bisecting. */
#define JOIN_BARRIER_GO_DELAY_MS 3u
/* vms-e81: how long after a newcomer's START completes before we open our own
 * client half to it. The pure-VMS control shows a member acting 0.24-1.67 s
 * after START, on a per-node ~1 s scan phase -- so this is a timer, not an
 * event hook. 1000 ms sits inside the observed band. */
#define JOIN_MEMBER_GREET_MS 1000u
/* vms-3aba (spec 4(O.27)): on a rejoin, how long to HOLD OVMX's own dir/MSCP
 * client half to a coordinator CANDIDATE (the highest node number known so far,
 * with no strictly-lower peer yet seen) before giving up and initiating anyway.
 * The two members complete their 0x41 START in a non-deterministic order; when
 * the coordinator (higher node) appears FIRST, cm_peer_is_coordinator() cannot
 * yet say it is the coordinator (its lower-peer guard is unsatisfied), so the
 * vms-9af lean-VC suppression fired ~1.3 s too LATE -- after own-dir had already
 * bloated the coordinator's VC and op 0x02 rode at send_seq 21/22, PAST the
 * coordinator's recv_ack ceiling of 18 (spec 4(O.26)). Holding the client half
 * for this settle window lets the lower (non-coordinator) member appear, so the
 * suppression engages BEFORE own-dir ever rides -- and op 0x02 lands under the
 * ceiling. The cap keeps a genuinely-lone existing member from being starved of
 * discovery forever: the observed inter-member START skew is ~1.3 s, so 6 s is a
 * wide margin while staying bounded. Kill-switch OVMX_REJOIN_LEAN_EARLY=0. */
#define LEAN_COORD_SETTLE_MS 6000u
/* vms-e81: minimum quiet gap before a non-ack START counts as a RE-START rather
 * than the ordinary round-1 of a handshake in progress. VAX3's re-STARTs were
 * ~5 s apart; the normal round-0/round-1 pair is back-to-back (sub-millisecond). */
#define START_RESTART_GAP_MS 4000u
/* vms-760: cap on how long we hold the deferred op 0x02 waiting for a peer we
 * have a channel to but no config from. Beyond this we proceed with whoever has
 * configured, rather than let one silent peer block the join indefinitely. */
#define JOIN_CFG2_MAX_WAIT_MS 20000u
/* vms-760: cat-0x04 ack cadence -- ack once per this many unacked coordinator
 * messages. SET TO 1 DELIBERATELY. The reference joiner batches at one per three
 * (85 acks for a 254-message burst), but our poll loop is HELLO-driven and far too
 * coarse to carry the accompanying idle-flush: with a threshold of 3, a burst that
 * arrives two messages at a time stalled waiting on a ~4-second poll tick and the
 * whole transaction crawled (d94-e10: 96 messages over 118 s). Acking every message
 * is legal -- an ack just names a watermark -- and keeps the burst at wire speed.
 * Restoring the batch needs a millisecond-resolution timer, not the HELLO poll.
 * Original note: the reference joiner BATCHES at one per three (85 acks for a
 * 254-message burst; frames 313/314/315 carry am=11/14/17), but batching alone
 * is not safe: it strands a trailing backlog below the threshold that nothing
 * ever flushes, and the coordinator then waits forever for an ack of its last
 * one or two messages (observed in d94-e6: it acked through 76, messages 77-78
 * arrived, and the transaction stopped dead). Acking every message is legal --
 * the ack simply names a watermark -- and cannot strand anything. Batching can
 * be restored once an idle-flush exists to accompany it. */
#define SCS_CM_ACK_EVERY     1u
/* Flush any residual ack backlog this long after the last ack. Well under the
 * cluster's own reconnection interval, so the coordinator never waits on us. */
#define SCS_CM_ACK_FLUSH_MS  15u

struct peer_state {
    struct scs_pb *pb;        /* Path Block: this peer's port + virtual circuit; NULL = free slot */
    /* vms-694: this peer's per-peer Con.ID low-word block offset (peer slot
     * index * OVMX_PEER_CONID_STRIDE). Slot 0 is 0 -- the historical fixed
     * class slots -- so single-peer captures are byte-identical; each further
     * concurrently-live peer gets a distinct block so two peers never collide
     * in the node-wide CDL. Set once at allocation (peer_find_or_add), used via
     * the PS_*_CONID(ps) macros. */
    uint16_t conid_off;
    /* vms-17f: CLOCK_MONOTONIC ms at which this peer was last heard from AT ALL
     * -- any 0x6007 frame carrying its source MAC, multicast beacons included,
     * not only the ones addressed to us. That breadth is deliberate: a node that
     * is still beaconing has not departed, and stamping only unicast-to-us
     * frames would declare a live-but-quiet member gone. 0 = never heard (a slot
     * built by a test, or by a lookup that allocated one). */
    uint64_t last_rx_ms;
    int      channel_up;      /* >=1 directed HELLO exchanged (NISCA channel, not an SCA VC state) */
    long     directed_replies;
    struct timespec last_directed; /* CLOCK_MONOTONIC of our last directed reply (rate limit) */
    int      connect_sent;    /* we sent a CONNECT-REQUEST */
    int      connected;       /* Con.ID pair bound */
    uint32_t remote_conid;    /* peer's Con.ID once learned */
    /* --- vms-af2/vms-691: node-incarnation echo (the established-join gate) ---
     * The member advertises the incarnation it attributes to OVMX in its
     * directed-HELLO flag at abs 92 ([78:80]); OVMX echoes it into its 0x41
     * START [22:24] and its own directed-HELLO abs 92 (spec sec 4i.B). Read off
     * the wire, never hard-coded. 0 = not yet observed (fall back to 1, the
     * fresh-contact value). */
    uint16_t incarnation;
    /* --- vms-21e / vms-691: SCS sequenced-message VC engine state --- */
    struct scs_vc vc;              /* seq/ack tracking + credit + retransmit (embeds scs_seq_state) */
    int      start_replied;        /* we answered the peer's 0x41 START (sent round 0/1) */
    int      start_initiated;      /* vms-d60: WE issued the round-0 START first (member role,
                                    * OVMX_MCAST_SOLICIT), rather than only reflecting a peer's */
    int      start_acked;          /* we sent the round-2 46-byte ack -> START complete */
    int      vc_noack_warned;      /* vms-694: SCSD-W-VCNOACK already emitted for this peer */
    long     start_replies;        /* count of 0x41 frames we sent to this peer */
    long     credit_sent;          /* vms-691: 0x48 credit-returns we sent this peer */
    /* --- vms-246: SCS$DIRECTORY / SCS$DIR_LOOKUP responder state --- */
    int      dir_seen;             /* the peer has begun the directory exchange with us */
    int      dir_connected;        /* SCS$DIRECTORY Con.ID pair bound (we sent CONNECT-RESPONSE) */
    uint32_t poll_remote_conid;    /* vms-66f: the peer's handle on the connection OUR
                                    * SCS$DIR_LOOKUP opened to ITS SCS$DIRECTORY.
                                    * Distinct from dir_remote_conid, which is the
                                    * peer-opened direction (p. 2-49). */
    uint32_t dir_remote_conid;     /* the peer's SCS$DIRECTORY handle (learned from its request) */
    long     dir_lookups_answered; /* SCS$DIR_LOOKUP responses we sent this peer */
    /* --- vms-224: connection-manager add-member SYSAP dialogue (spec sec 4j) ---
     * The 190-byte VMS$VAXcluster VC carries a SECOND, application-level
     * send/ack-msg# counter pair (body[0:4]) distinct from the SCS VC seq. */
    int      cm_config_sent;       /* we sent our op 0x14/0x01/0x02 config burst */
    uint16_t sysap_send;           /* OVMX's next SYSAP send-msg# (body[0:2]; from 1) */
    uint16_t sysap_recv;           /* high-water of the member's SYSAP send-msg# (ack target) */
    /* vms-584 stray-ack instrumentation: WHEN the high-water last advanced, and
     * WHICH frame advanced it. Only ever read to describe an ack after the fact
     * -- never to decide whether to send one (see cm_send_ack). */
    long     cm_recv_advanced_ms;
    uint8_t  cm_last_recv_cat;
    uint8_t  cm_last_recv_op;
    /* vms-584: cluster facts read out of a transition-OPEN, held until the
     * transition is real (our barrier completes; immediately for class 0x04). */
    int      pending_state;
    uint16_t pending_members;
    uint64_t pending_last_transition;
    long     cm_responses;         /* 0x81 responses we sent to member 0x03/0x05 txns */
    /* --- vms-d94: ACTIVE-JOINER VMS$VAXcluster connection (the connection OVMX
     * OPENS to the member, distinct from the member-opened one above). The
     * clean-ref join sends the add-member burst on THIS joiner-initiated
     * connection (formation-clean-2node.pcap idx52 CONNECT-REQ -> idx59 CM). */
    int      joiner_connect_sent;  /* we sent our own VMS$VAXcluster CONNECT-REQUEST */
    int      joiner_connected;     /* the member accepted it (Con.ID pair bound) */
    /* vms-164d (DLM rung-1b): the LIVE cross-node $ENQ round-trip state. */
    int      dlm_enq_sent;         /* node-A: we sent one OVMX$DLM ENQ to this peer (one-shot) */
    int      dlm_grant_recv;       /* node-A: the peer's GRANT response arrived */
    uint32_t dlm_grant_status;     /* node-A: the VMS status the peer's executive returned */
    unsigned long dlm_srv_responses; /* node-B: GRANT frames we emitted back to this peer */
    /* vms-6ca (DLM rung H5): node-A's block-then-grant-over-the-wire sequence.
     * A holds RESONE EX (req_lkid 1), then a SECOND incompatible $ENQ (req_lkid 2)
     * QUEUES on B; A releases #1; B WIRES the deferred GRANT and A's origin record
     * for #2 flips NL->EX. */
    int      dlm_h5;               /* node-A: H5 sequence armed (OVMX_DLM_H5) */
    uint32_t dlm_master_lkid1;     /* node-A: B's master handle for our holder (#1) */
    int      dlm_enq2_sent;        /* node-A: the second (contending) $ENQ sent */
    int      dlm_deq1_sent;        /* node-A: the holder's $DEQ sent */
    int      dlm_pend_seen;        /* node-A: the queued-reply for #2 landed (pending) */
    int      dlm_flip_seen;        /* node-A: #2's origin record flipped to granted */
    /* vms-76d (DLM rung H6): the BLKAST WIRE, layered on H5. When armed, node A
     * establishes its holder (#1) WITH a blocking-AST routine; node B, on queuing
     * the incompatible #2, WIREs a BLKAST to A; A RECEIVES it and FIRES a real
     * blocking AST on its own executive (drained via DELIVERAST), THEN releases #1
     * -- the holder releases in genuine RESPONSE to the BLKAST, not on its own. */
    int      dlm_h6;               /* node-A: H6 BLKAST-wire armed (OVMX_DLM_H6) */
    int      dlm_holder_armed;     /* node-A: holder (#1) origin established w/ blkast */
    int      dlm_blkast_fired;     /* node-A: a received BLKAST fired a real AST here */
    int      dlm_blksent;          /* node-B: a BLKAST was WIREd to the holder */
    /* rd vms-d81 (DLM rung H8): the LVB WIRE. Node A holds RESONE/RLVB EX
     * (req_lkid 1), then WRITES a known 16-byte value block and releases with a
     * cross-node $DEQ carrying LCK_M_VALBLK; the master (node B) replicates the
     * wire value into res->valblk (vms_lock_dlm_xnode_deq) and a LOCAL $ENQ on B
     * reads back the value A wrote. Independent of H5/H6 -- no contention, no
     * BLKAST; a single ENQ then a value-block-carrying $DEQ. */
    int      dlm_h8;               /* node-A: H8 LVB-wire armed (OVMX_DLM_H8) */
    int      dlm_h8_read_done;     /* node-B: the LOCAL LVB read-back ran (one-shot) */
    /* vms-eeb (DLM rung H9): the LVB READ crossing -- the MIRROR of the H8 write
     * crossing (vms-d81). Node B (the master) SEEDS a known 16-byte value block
     * into RRD's resource value block; node A does a cross-node $ENQ (EX, VALBLK),
     * the master's GRANT carries its res->valblk back, and node A reads it via a
     * LOCAL GETLKI on its OWN request handle -- the master's LVB, over the wire. */
    int      dlm_h9;               /* node-A: H9 LVB-read crossing armed (OVMX_DLM_H9) */
    int      dlm_h9_read_done;     /* node-A: the LVB read-back completed (one-shot) */
    /* rd vms-dca9 (DLM rung H10b): the REMASTER LOCK REBUILD. Node A holds
     * RES_C EX (req_lkid 1, mastered by C) BEFORE C departs; when C departs
     * gracefully, A re-registers that SAME lock on RES_C's new directory
     * master -- a TARGETED SCS_DLM_OP_REBUILD send, resolved by peer_by_csid --
     * so the new master reconstructs res->granted from A's REAL origin state.
     * Independent of H5/H6/H8/H9. */
    int      dlm_h10b;                  /* node-A: H10b hold+rebuild armed (OVMX_DLM_H10B) */
    int      dlm_h10b_hold_established; /* node-A: our pre-departure hold is GRANTED */
    char     dlm_h10b_resnam[32];       /* node-A: the resource name we hold (from the GRANT) */
    int      dlm_h10b_rebuild_sent;     /* node-A: the REBUILD send ran (one-shot) */
    /* rd vms-ec75 (DLM rung H11): DISTRIBUTED DEADLOCK SEARCH. A contender node
     * holds one resource (mastered by C) and $ENQs a second that QUEUES behind the
     * other contender -- a genuine cross-node wait-for cycle. When the wait queues,
     * the MASTER (C) initiates an edge-chasing DLKSRCH probe that detects the cycle
     * over the two readback authorities (DLM_ENUM_WAITS home + DLM_GET_GRANTED
     * master) and aborts the GLOBAL-min victim with SS$_DEADLOCK. These fields drive
     * a contender's timed hold-then-wait sequence to C. */
    int      dlm_ec75;                  /* contender: H11 armed (OVMX_DLM_EC75) */
    uint64_t dlm_ec75_join_ms;          /* contender: ms clock when the DLM VC to C was ready */
    int      dlm_ec75_hold_sent;        /* contender: the HOLD $ENQ (#1) was sent to C */
    int      dlm_ec75_wait_sent;        /* contender: the WAIT $ENQ (#2) was sent to C */
    int      dlm_ec75_victim_seen;      /* contender: our WAIT $ENQ returned SS$_DEADLOCK */
    int      dlm_ec75_search_sent;      /* master(C): the one-shot search was initiated  */
    uint32_t dlm_ec75_init_conid;       /* master(C): the initiator's DLM client conid --
                                         * the unprompted victim GRANT's dest (learned
                                         * from the initiator's WAIT $ENQ frame). */
    uint32_t dlm_ec75_init_csid;        /* master(C): the initiator's CSID              */
    char     dlm_ec75_init_resnam[32];  /* master(C): the resource the initiator waits on */
    /* vms-694 (§4(O.7)): OVMX's OWN model of "am I a connected VMS$VAXcluster
     * member of this peer", LATCHED the moment the VMS$VAXcluster SYSAP
     * connection (cdt_joiner, or cdt_member if it ever reaches OPEN) hits the
     * Figure 2-14 OPEN state. It is a LATCH, not a live read, on purpose: the
     * graceful DISCONNECT the daemon issues at shutdown legitimately drives the
     * CDT OPEN->...->CLOSED, so a state SAMPLED at teardown reads CLOSED even
     * though the connection was open and carrying SYSAP data all through the run
     * (proven live, §4(O.6)/§4(O.7)). Sampling the CDT at exit is what made the
     * exit summary read joiner=CLOSED and misled readers into "OVMX never models
     * the connection". This latch is the honest membership fact and it SURVIVES
     * the teardown sample. Cleared only on a genuine re-START session reset. */
    int      vaxcluster_open_reached;
    /* vms-c7d: this peer's Cluster System Block connectivity slice -- the
     * CSB state machine (transcript pp. 7-23/7-24) and the RECNXINTERVAL
     * reconnect bookkeeping (p. 7-30). OPEN when the VMS$VAXcluster connection
     * latches, WAIT (membership HELD) on a non-last-gasp VC break, driven once
     * per second by scsd_recnx_tick(). See scs_recnx.h. */
    struct scs_csb csb;
    uint32_t joiner_remote_conid;  /* the member's Con.ID on OUR connection (from its response) */
    int      joiner_cm_sent;       /* we sent the add-member burst on our own connection */
    long     joiner_cm_ms;         /* monotonic_ms() when that burst went out */
    int      joiner_cfg2_sent;     /* vms-760: the DEFERRED op 0x02 config/topology went out */
    int      rejoin_credit_first_sent; /* vms-46f: OVMX's op-6 special-credit request rode the
                                    * coordinator's SCS$DIRECTORY connection AHEAD of op 0x02
                                    * (spec 4(O.17), Davis pp. 2-43/2-44). Gates the deferred
                                    * op 0x02 on a rejoin so op-6 provably precedes it. */
    uint16_t sysap_acked;          /* vms-760: highest peer send-msg# we have cat-0x04 acked */
    long     cm_acks;              /* cat-0x04 acks emitted to this peer */
    long     cm_last_ack_ms;       /* monotonic_ms() of our last cat-0x04 ack */
    /* vms-760: cluster-wide state-transition barrier (spec 4p). */
    uint32_t barrier_epoch;        /* body[12:16] latched from the coordinator's op 0x09 */
    uint8_t  xition_class;         /* vms-e4b: body[17] of the transition-open in
                                    * flight -- 0x02 add, 0x03 remove-failed,
                                    * 0x04 self-departure. 0 = none seen yet, and
                                    * we then answer op 0x12 as class 0x02, which
                                    * is exactly what the proven join path did. */
    int      barrier_step;         /* current step N, 1..12; 0 = barrier not running */
    int      barrier_done;         /* our OWN admission finished (record, NOT a gate) */
    unsigned barrier_count;        /* transitions completed; #1 = our join, 2+ = bystander */
    /* vms-197 (spec §4(O.38)): count of cat-0x01 op 0x06 MEMBERSHIP frames
     * RECEIVED from this peer -- the coordinator's post-commit membership
     * publication. Grounded 1:1 against the member oracle on a fresh five-return
     * sweep (virgin vaxlab-3, §4(O.38) captures o38o-r1..r5): 254 on every
     * member-oracle WIN (CLUSTER_NODES 2->3 on BOTH members, incl. the phantom
     * r3 that completed only 4 of 12 barrier steps so emitted no XITDONE) and 0
     * on the genuine LOSE (r4). This is the AUTHENTIC "OVMX is now a committed
     * cluster member" signal -- the coordinator only publishes it once the
     * transition commits cluster-wide (Rule of Total Connectivity, Davis p.7-39),
     * which a LOSE never reaches. RECEIVE-ONLY: OVMX's wire behaviour is
     * unchanged; this counter and the latch below only correct what OVMX
     * CONCLUDES about its OWN admission (diagnostics/self-report). */
    long     membership_bursts;
    long     last_start_ms;        /* vms-e81: monotonic_ms() of the last 0x41 START
                                    * we processed from this peer -- a genuine
                                    * re-START is separated by a QUIET GAP, not by
                                    * its round number (rounds 0 and 1 are both
                                    * non-ack and arrive back-to-back). */
    long     greet_due_ms;         /* vms-e81: when to open OUR client half to a
                                    * newcomer. The control shows a real member
                                    * acts on a ~1 s periodic scan after START
                                    * completes (observed 0.24-1.67 s), NOT
                                    * instantly -- and firing from the receive
                                    * path is how we lost the barrier race. */
    int      appeared_after_join;  /* vms-e81: first seen AFTER our own admission --
                                    * this peer is a NEWCOMER to a cluster we are
                                    * already in, so the MEMBER role applies, never
                                    * the joiner sequencer. */
    /* vms-760: WE ARE TOO FAST. OVMX answers the coordinator's op 0x0a GO in
     * ~20 us -- quicker than the coordinator's own fan-out loop, which takes
     * 20-214 us per additional member. A step-1 op 0x0b that lands while the
     * coordinator is still fanning 0x0a out to the other members is acked with
     * tag 0x0260 and response marker 0x00 ("received in the go phase, NOT
     * counted") instead of 0x0210 / 0x01, and the barrier stays a member short
     * forever. GROUNDED 6/6 across every 3-member run; epoch 5 produced BOTH
     * outcomes, so it is a race, not cluster state. So the step-1 request is
     * DEFERRED by barrier_go_ms + JOIN_BARRIER_GO_DELAY_MS instead of being sent
     * from the receive path. */
    long     barrier_go_ms;        /* monotonic_ms() when the op 0x0a GO arrived */
    int      barrier_go_pending;   /* step-1 op 0x0b is deferred, not yet sent */
    uint16_t own_txn;              /* our per-VC transaction-context id */
    uint16_t own_cksum;            /* our per-VC counter, +1 per transaction we initiate */
    uint32_t cm_local_conid;       /* our Con.ID on the VC the CM dialogue rides */
    uint32_t cm_remote_conid;      /* the peer's Con.ID on that VC */
    /* vms-760: WHICH virtual circuit carried our config burst -- the VC we opened
     * (OVMX_JOINER_CONID) normally, or the one the MEMBER opened (OVMX_LOCAL_CONID)
     * under OVMX_NO_OWN_VC. The deferred op 0x02 must ride the SAME VC. */
    int      cfg_sent;             /* a config burst has gone out on this peer */
    long     cfg_ms;               /* monotonic_ms() when it did */
    uint32_t cfg_local_conid;      /* our Con.ID on that VC */
    uint32_t cfg_remote_conid;     /* the member's Con.ID on that VC */
    uint16_t joiner_req_seq;       /* the send_seq of our CONNECT-REQUEST (retransmits REUSE it) */
    struct timespec last_joiner_req; /* CLOCK_MONOTONIC of our last joiner CONNECT-REQUEST (retx) */
    /* --- vms-760: OVMX's OWN SCS$DIRECTORY connection to the member (the active
     * joiner opens its own directory + queries the member, clean-ref idx25/32).
     * Distinct from the member-opened directory connection (dir_* above). --- */
    int      own_dir_sent;         /* we sent our own SCS$DIRECTORY CONNECT-REQUEST */
    int      own_dir_connected;    /* the member accepted it (pair bound) */
    uint32_t own_dir_remote_conid; /* the member's handle on OUR directory connection */
    int      own_dir_lookup_sent;  /* we sent our VMS$VAXcluster lookup-request on it */
    uint16_t own_dir_req_seq;      /* send_seq of our dir CONNECT-REQUEST (retransmits REUSE it) */
    struct timespec last_own_dir;  /* CLOCK_MONOTONIC of our last own-dir send (retx rate-limit) */
    int      lean_vc_suppressed;   /* vms-9af: this peer is the rejoin COORDINATOR and its
                                    * VC is kept LEAN -- OVMX's OWN dir/MSCP client half is
                                    * suppressed here so op 0x02 rides at a low send_seq.
                                    * Latched once, to log SCSD-I-LEANVC exactly once. */
    long     lean_hold_start_ms;   /* vms-3aba (spec 4(O.27)): monotonic_ms() when we FIRST
                                    * held this coordinator CANDIDATE's own-dir client half
                                    * waiting for the topology to settle (a lower peer to
                                    * appear) so lean-VC engages BEFORE op 0x02. 0 = not held
                                    * yet. Bounded by LEAN_COORD_SETTLE_MS. */
    /* --- vms-760: OVMX's OWN MSCP$DISK client connection to the member
     * (VMS$DISK_CL_DRVR -> MSCP$DISK, clean-ref formation-clean-2node.pcap SCA
     * idx35 connect -> idx39 member accept). This is the connection OVMX has
     * never presented in any capture; the member reciprocates the add-member
     * config on the joiner VC only once the joiner presents this full
     * connection-set (NEW->MEMBER, spec sec 4L(7)). --- */
    int      mscp_connect_sent;    /* we sent our MSCP$DISK CONNECT-REQUEST */
    int      mscp_connected;       /* the member accepted it (Con.ID pair bound) */
    uint32_t mscp_remote_conid;    /* the member's MSCP handle on OUR connection (from its accept) */
    uint16_t mscp_req_seq;         /* send_seq of our MSCP CONNECT-REQUEST (retransmits REUSE it) */
    /* vms-760: MSCP disk-discovery on OUR bound MSCP$DISK client connection. The
     * reference joiner (vax3-2to3) runs SET CONTROLLER CHARACTERISTICS x2 then a
     * GET UNIT STATUS walk (NEXT-UNIT modifier) until an END returns OFFLINE --
     * VAX1 answers the add-member config only after this completes. */
    int      mscp_scc_sent;        /* how many SCC commands we have issued (0..2) */
    int      mscp_disc_done;       /* GUS walk hit the OFFLINE terminator */
    uint16_t mscp_msg_id;          /* incrementing correlation msg-id (echoed by VAX1) */
    uint16_t mscp_next_unit;       /* next GUS unit-word = last END's unit + 1 */
    struct timespec last_mscp_req; /* CLOCK_MONOTONIC of our last MSCP CONNECT-REQUEST (retx) */
    /* --- vms-760 SERVER-FIRST established-join: OVMX serves the MEMBER-OPENED
     * MSCP$DISK connect (member = VMS$DISK_CL_DRVR client, OVMX = MSCP$DISK
     * server). DISTINCT from the mscp_* CLIENT fields above (that is OVMX's own
     * outbound MSCP connect, driven only by the sequencer). This server path is
     * additive and runs on the DEFAULT path. --- */
    int      mscp_srv_bound;       /* we sent the op=2 ACCEPT_REQ binding OUR server handle (vms-257) */
    int      mscp_srv_confirmed;   /* the server FSM reached OPEN off the member's op=3 ACCEPT_RSP (vms-257) */
    uint32_t mscp_srv_remote_conid;/* the member's MSCP CLIENT Con.ID (its local in its op=0 connect) */
    long     mscp_srv_accepts;     /* op=4 accepts we sent this peer */
    uint16_t mscp_srv_echo_seq;    /* send_seq allocated for the op=1 echo (reused on retransmit) */
    uint16_t mscp_srv_accept_seq;  /* send_seq allocated for the op=4 accept (reused on retransmit) */
    /* --- vms-760 PURE-SERVER disk-CLIENT: after OVMX reaches NEW as a pure
     * server it opens its OWN SCS$DIRECTORY + MSCP$DISK connections back to VAX1
     * (distinct PS conids) and runs MSCP disk discovery (SCC + GUS enumeration).
     * A SEPARATE state machine from the OVMX_JOIN_SEQ sequencer -- it NEVER opens
     * a VMS$VAXcluster VC. All sends ride the shared per-channel ps->vc.seq;
     * connect/lookup seqs are allocate-once/retransmit-reuse (the 760mscp hole). */
    int      psc_step;             /* enum ps_client_step; current stop-and-wait state */
    /* psc_credit_done STOOD HERE AND IS DELETED (vms-096). Its ONE writer was
     * the `cm_op == 6` block nested inside `cm_op == 8`, which is unreachable,
     * so the flag was 0 for the whole life of every process. Its two readers
     * are gone with it: the immediate disk-discovery trigger it gated (dead by
     * the same argument) and a `psc_credit_done ||` term in main()'s ungate
     * that could never be true. Keeping a field that is structurally always 0
     * is how a condition reads as "sometimes" in a diff. */
    /* vms-2f3 sec 4M.20: allocate-once/retransmit-reuse for the op6/op8
     * credit-handshake REPLY, the same rule the struct header states for
     * connect/lookup seqs and that psc_dir_req_seq already implements.
     * scs_reflect_credit() advanced ps->vc.seq unconditionally, so once sec
     * 4M.14 made us answer retransmissions we began answering the SAME request
     * with a NEW sequence number every time (observed 12 -> 13 -> 14 against a
     * peer replaying send_seq=12). A retransmit must be answered by replaying
     * the original reply. */
    struct scs_retx_seq credit_seq; /* see scs_retx_reply_seq() in scs_vc.h */
    int      psc_self_disc_sent;   /* vms-2f3 sec 4M.23: we performed our OWN disconnect
                                    * call (op 6) after the peer's never came */
    int      psc_dir_sent;         /* we sent OUR PS SCS$DIRECTORY CONNECT-REQUEST */
    int      psc_dir_connected;    /* VAX1 accepted it (op2, pair bound) */
    uint32_t psc_dir_remote_conid; /* VAX1's handle on OUR PS dir connection */
    uint16_t psc_dir_req_seq;      /* send_seq of OUR PS dir CONNECT-REQUEST (retx REUSE) */
    uint16_t psc_lookup_seq;       /* send_seq of the outstanding PS dir lookup (retx REUSE) */
    int      psc_mscp_sent;        /* we sent OUR PS MSCP$DISK CONNECT-REQUEST */
    int      psc_mscp_connected;   /* VAX1 accepted it (op2, pair bound) */
    uint32_t psc_mscp_remote_conid;/* VAX1's MSCP$DISK server handle on OUR connection */
    uint16_t psc_mscp_req_seq;     /* send_seq of OUR PS MSCP CONNECT-REQUEST (retx REUSE) */
    uint16_t psc_mscp_msgid;       /* MSCP message-id (SCC/GUS correlation token; increments) */
    long     psc_scc_sent;         /* SET CONTROLLER CHARACTERISTICS commands sent (af2 sends 2) */
    long     psc_gus_sent;         /* GET UNIT STATUS commands sent */
    long     psc_gus_avail;        /* UNIT AVAILABLE (status 4) responses seen */
    char     psc_lookup_name[16];  /* SYSAP name of the outstanding PS lookup (for retx rebuild) */
    struct timespec psc_last_tx;   /* CLOCK_MONOTONIC of the outstanding PS drive frame */
    /* vms-2f3 step 4: monotonic ms at which the CM config exchange completed.
     * The disk-discovery ungate times out against this -- see the PSCUNGATE
     * block in the main loop. Set unconditionally next to cm_config_sent,
     * NOT reusing cfg_ms, which is only stamped when a frame actually went
     * out and is therefore 0 on the pure-server path. */
    uint64_t psc_gate_ms;
    unsigned psc_retx;             /* retransmits of the current PS step (cap = JOIN_RETX_MAX) */
    /* --- vms-760: the NEW->MEMBER join SEQUENCER. The joiner presents the FULL
     * dir-CLIENT connection-set in strict stop-and-wait order on the ONE shared
     * per-channel send_seq (docs/design-cluster-join-choreography.md): own dir
     * connect -> confirm -> lookup MSCP$TAPE(miss) -> lookup MSCP$DISK(hit) ->
     * MSCP$DISK connect -> lookup VMS$VAXcluster(hit) -> VC connect -> add-member
     * burst. Each connect is issued ONLY after its lookup HIT, so no frame the
     * member cannot process ever enters the shared sequence (the 760b/760mscp
     * hole). Exactly one join-drive frame outstanding; retransmits REUSE seq. --- */
    int      join_step;            /* enum join_step; current stop-and-wait state */
    uint16_t js_lookup_seq;        /* send_seq of the outstanding lookup-request (retx REUSE) */
    char     js_lookup_name[16];   /* SYSAP name of the outstanding lookup (for retx rebuild) */
    struct timespec js_last_tx;    /* CLOCK_MONOTONIC of the outstanding join-drive frame */
    unsigned js_retx;              /* retransmits of the current step (cap = JOIN_RETX_MAX) */
    /* --- vms-9f3: NISCA channel packet-size verification (padded directed
     * HELLO, spec sec 4k). An ESTABLISHED VAX1 zero-pads a directed HELLO up to
     * NISCS_MAX_PKTSZ and retransmits it (1500->1069->853->745, ~6s) until OVMX
     * RECIPROCATES on the reverse channel; that reciprocal is what lets VAX1
     * open OVMX's CSB and drive the sec 4j add-member commit. */
    long     padded_replies;       /* padded HELLOs we sent this peer (reciprocations) */
    struct timespec last_padded;   /* CLOCK_MONOTONIC of our last padded send (rate limit) */
    int      padded_initiated;     /* we proactively sent one padded HELLO (golden joiner-first) */
    uint16_t peer_padded_sca;      /* largest padded-HELLO total SCA size seen from the peer */
    /* --- vms-dd5: the three CONNECTION DESCRIPTORS this peer's three SCS
     * connections live in, and with them the documented connection state
     * machine (scs_conn.h). Each is a CDT in the node-wide Connection
     * Descriptor List (scsd_cdl), claimed at the EXACT Con.ID OVMX has been
     * putting on the lab wire since vms-5fe so that wiring the CDL changes no
     * byte. NULL = not yet formed, or the machine is off (OVMX_NO_CONN_FSM), or
     * the CDL slot was already claimed by another peer -- see conn_bind(). The
     * booleans above (dir_connected/connected/joiner_connected) are NOT
     * replaced: they still gate every send, so the machine is a recorder, not a
     * gate. */
    struct scs_cdt *cdt_dir;    /* SCS$DIRECTORY,   local Con.ID = PS_SCS_DIR_CONID(this peer) */
    struct scs_cdt *cdt_member; /* VMS$VAXcluster the MEMBER opened, local = PS_LOCAL_CONID(this peer) */
    struct scs_cdt *cdt_joiner; /* VMS$VAXcluster OVMX opened,       local = PS_JOINER_CONID(this peer) */
    /* vms-abc: has send_frame_vc() already announced that THIS circuit carries
     * no traffic? The refusal is a per-circuit fact, not a per-frame one, and
     * the daemon re-enters the refusing paths at the peer's HELLO cadence
     * (~1 Hz), so logging every refusal buries the run in identical lines.
     * Cleared by scsd_vc_on_open() so a circuit that breaks AGAIN says so
     * again. Every refusal is still COUNTED in vc_sends_refused and reported
     * by scsd_exit_summary(). */
    int      novc_logged;
};

static int mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

/*
 * is_padded_hello - vms-9f3: is `buf` (n bytes on the wire) a padded directed
 * HELLO (spec sec 4k)? A HELLO-family frame -- message-class byte 0x05 at abs 36
 * and NOT the 0x13 SCS-envelope format constant at abs 31 (this is a HELLO, not
 * an 0x4b sequenced message) -- whose total SCA content EXCEEDS a normal
 * 120-byte HELLO, up to NISCS_MAX_PKTSZ (+2). Per sec 4k the distinguishing
 * feature of a padded HELLO is its SIZE plus the HELLO class byte; the pad tail
 * is pure zeros. The frame is already known to be unicast to our HW MAC (gated
 * by the caller).
 */
static int is_padded_hello(const uint8_t *buf, size_t n)
{
    return n > (size_t)SCS_HELLO_FRAME_LEN &&
           n <= (size_t)SCS_HELLO_PADDED_MAX_FRAME &&
           buf[31] == 0x00 &&   /* HELLO family, NOT the 0x13 SCS envelope (sec 4k) */
           buf[36] == 0x05;     /* message-class byte 0x05 == HELLO (spec sec 4a/4k) */
}

/* vms-7be: all-zero address returned by the accessors below when the structure
 * they read is not built yet -- byte-for-byte what the old zero-initialized
 * peer_state fields produced in the same situation. */
static const uint8_t zero_addr[6] = {0, 0, 0, 0, 0, 0};

/* The peer's Ethernet PORT address (PB, p. 2-12) -- where we send its frames. */
static const uint8_t *ps_port_addr(const struct peer_state *ps)
{
    return (ps != NULL && ps->pb != NULL) ? ps->pb->remote_port_addr : zero_addr;
}

/* The peer's 48-bit SCS System Address (SB, p. 2-16) -- the cluster-logical
 * address OVMX writes in the SCA dest-logical field. Zeros until discovered,
 * exactly as the old peer_state.logical was. */
static const uint8_t *ps_sys_addr(const struct peer_state *ps)
{
    return (ps != NULL && ps->pb != NULL && ps->pb->sb != NULL) ? ps->pb->sb->system_id
                                                                : zero_addr;
}

/*
 * peer_find_or_add - vms-7be: look the peer up by its remote PORT ADDRESS in
 * the configuration structures (PDT formative queue + open PBs, p. 2-21) and,
 * on first contact, build the Path Block that describes the newly discovered
 * port and the circuit forming with it (p. 2-11). The peer_state slot is now
 * just the index cell that hangs the connection-level state off that PB.
 */
static struct peer_state *peer_find_or_add(struct scs_config *cfg, struct scs_pdt *pdt,
                                           struct peer_state *tbl, const uint8_t mac[6])
{
    int free_slot = -1;
    int we_are_member = 0;
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        if (tbl[i].pb != NULL && mac_eq(tbl[i].pb->remote_port_addr, mac)) {
            return &tbl[i];
        }
        /* vms-e81: have we already completed our OWN admission with anyone? If
         * so, a peer we are meeting for the first time is a NEWCOMER joining a
         * cluster we are already in -- the member role, not the joiner role.
         * vms-578: slot occupancy is `pb != NULL` (vms-7be), not the retired
         * `in_use` flag -- a slot has a Path Block exactly when it is in use. */
        if (tbl[i].pb != NULL && tbl[i].barrier_done) {
            we_are_member = 1;
        }
        if (tbl[i].pb == NULL && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return NULL;
    }
    struct scs_pb *pb = scs_pb_create(cfg, pdt, mac, SCS_PORT_TYPE_ETHERNET);
    if (pb == NULL) {
        return NULL; /* PB pool exhausted -- same "no room" answer as before */
    }
    memset(&tbl[free_slot], 0, sizeof(tbl[free_slot]));
    tbl[free_slot].pb = pb;
    /* vms-694: assign this peer's per-peer Con.ID block. Slot index * stride,
     * so slot 0 keeps offset 0 (the historical fixed class slots) and every
     * other concurrently-live peer gets a distinct 0x10-wide block -- the fix
     * for the node-global Con.ID collision that dropped a 2nd member's CM
     * traffic as SCSD-W-RXSRCMISMATCH. Stable for the life of the slot; a
     * departed slot reused later gets the same offset, and its old CDTs were
     * released by scs_pb_depart(). */
    tbl[free_slot].conid_off = (uint16_t)((unsigned)free_slot * OVMX_PEER_CONID_STRIDE);
    /* vms-c7d: this peer's CSB starts NEW (p. 7-24). Seeded with the local
     * RECNXINTERVAL so a later VC break sizes its reconnect period correctly. */
    scs_csb_init(&tbl[free_slot].csb, /*is_local=*/0, scsd_recnxinterval());
    /* vms-17f: first contact IS contact. Without this stamp a slot allocated by
     * a frame that never reaches peer_touch() would sit at last_rx_ms 0 forever
     * and the departure sweep would have nothing to age it from. */
    tbl[free_slot].last_rx_ms = monotonic_ms();
    /* vms-e81 (kept): role at first contact. The PB already carries `mac` in
     * remote_port_addr (scs_pb_create above), so the retired eth_mac copy is
     * gone -- ps_port_addr() reads it from the PB. */
    tbl[free_slot].appeared_after_join = we_are_member;
    return &tbl[free_slot];
}

/*
 * peer_touch - vms-17f: record that `mac` was heard from at `now_ms`. Stamps an
 * EXISTING peer slot only; it never allocates one, so a stray frame from an
 * unknown node cannot consume a slot. Called for every 0x6007 frame the daemon
 * receives, BEFORE the "unicast to our HW MAC" gate -- see the last_rx_ms field
 * comment for why the multicast beacons have to count.
 */
static void peer_touch(struct peer_state *tbl, const uint8_t mac[6], uint64_t now_ms)
{
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        if (tbl[i].pb != NULL && mac_eq(tbl[i].pb->remote_port_addr, mac)) {
            tbl[i].last_rx_ms = now_ms;
            return;
        }
    }
}

/*
 * peer_node_number - the peer's DECnet node number, taken from the SCA logical
 * address it advertises in its HELLO (abs 24-29, spec sec 4a). The DECnet
 * address lives in the low two bytes, little-endian: aa:00:04:00:<lo>:<hi>,
 * where the value is (area << 10) | node. VAX1 aa:00:04:00:01:04 -> 0x0401,
 * VAX2 ...:02:04 -> 0x0402, VAX3 ...:03:04 -> 0x0403.
 */
static uint16_t peer_node_number(const struct peer_state *ps)
{
    return (uint16_t)(ps_sys_addr(ps)[4] | ((uint16_t)ps_sys_addr(ps)[5] << 8));
}

/* Forward decl: DLM rung H10a graceful-departure directory ingress (rd vms-2bf).
 * DEFINED beside the other DLM drives (near the H9 seed/read helpers); CALLED
 * from the class-0x04 self-departure receive path far above that definition. */
static void scsd_dlm_h10_depart_ingress(struct peer_state *ps);

/* Forward decl: DLM rung H10b remaster lock rebuild, holder side (rd vms-dca9).
 * Same shape as the H10a forward decl above -- DEFINED beside the other DLM
 * drives, CALLED from the SAME class-0x04 self-departure receive path,
 * immediately after scsd_dlm_h10_depart_ingress(). */
static void scsd_dlm_h10b_rebuild_on_depart(struct peer_state *ps);

/*
 * cm_pick_coordinator - vms-760: choose the ONE peer that receives our deferred
 * op 0x02.
 *
 * GROUNDED (d94-e15, byte-verified): a NON-COORDINATOR peer silently DISCARDS
 * op 0x02. In e15 all three members received a byte-identical op 0x02 within
 * 400 ms; VAX1 and VAX2 answered only a cat-0x04 ack and did nothing further
 * (VAX1 had a 383 ms head start), while VAX3 relayed op 0x12 to VAX1 1.0 ms
 * later and drove the whole transition. The reference joiner behaves the same
 * way: it sent its op 0x02 to VAX2, not VAX1, and VAX2 was the coordinator
 * (vax3-2to3-established-join-20260730.pcap frame 285 -> 286 relay in 0.3 ms).
 * So the reference picks THE COORDINATOR, not "one peer arbitrarily", and our
 * old "first eligible peer" rule picked VAX1 -- exactly the wrong node (d94-e14:
 * acked, then nothing, CSID 00000000).
 *
 * HOW the joiner identifies the coordinator is NOT grounded. No wire-visible
 * coordinator flag was found: the byte-diff across both captures showed the
 * coordinator is a ZERO-VOTE node in each, and the fields that distinguish VAX3
 * from VAX1/VAX2 in our lab are all-zero on the reference's coordinator VAX2,
 * so they are node-local properties, not a role marker. The only predicate that
 * survives BOTH specimens is "highest DECnet node number", which is confounded
 * with "highest SCSSYSTEMID" and "last to have joined the cluster" -- all three
 * agree on VAX3 here and on VAX2 in the reference. We implement the observable
 * one and say so; see spec 5(z).
 *
 * OVMX_CFG2_PEER=<n> forces a specific DECnet node number, for bisecting.
 * Returns NULL if no peer has completed its config exchange yet.
 *
 * vms-2f3 2026-08-01 -- TWO CORRECTIONS, from SDA rather than from captures.
 *
 * (1) THE CLUSTER HAS A COORDINATOR FIELD AND IT ROTATES. SDA's Cluster Block
 * carries `Curr. coord. CSID`, and it changes with every transition: VAX3
 * (00010007) during run r1B, VAX1 (00010001) during s3B/s3C/s4A after VAX1
 * coordinated a removal. So "the coordinator" is a real, readable role and the
 * heuristic below cannot be tracking it -- it always answers VAX3.
 *
 * (2) AND IT IS A RESULT, NOT AN ADDRESS. Run s4A: a FRESH identity sent its
 * op 0x02 to VAX3 while SDA said the coordinator was VAX1, and was admitted in
 * 27 s -- with VAX3 proposing the addition and thereby BECOMING the
 * coordinator of that transition. The node you ask is the node that runs it.
 *
 * So do NOT "fix" this by chasing `Curr. coord.`: run s3C forced the op 0x02 to
 * VAX1, the actual coordinator, on an identity s3B had just been refused for,
 * and it was refused identically. Routing is not the rejoin gate, in either
 * direction. What d94-e15 saw (VAX1 and VAX2 acking a byte-identical op 0x02
 * and doing nothing while VAX3 drove it) is real but is NOT explained by
 * `Curr. coord.`, and the predicate behind it is still ungrounded -- see
 * spec 5(z). This heuristic stays, still labeled as a heuristic.
 */
static struct peer_state *cm_pick_coordinator(struct peer_state *tbl)
{
    const char *force = getenv("OVMX_CFG2_PEER");
    struct peer_state *best = NULL;
    uint16_t best_nn = 0;

    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        struct peer_state *ps = &tbl[i];
        if (!(ps->pb != NULL) || !ps->cfg_sent) {
            continue;
        }
        uint16_t nn = peer_node_number(ps);
        if (force != NULL) {
            if (nn == (uint16_t)strtoul(force, NULL, 0) ||
                (nn & 0x03ff) == (uint16_t)strtoul(force, NULL, 0)) {
                return ps;
            }
            continue;
        }
        if (best == NULL || nn > best_nn) {
            best = ps;
            best_nn = nn;
        }
    }
    return best;
}

/* vms-760: what we are allowed to SEND back for a token-correlated request. */
#define CM_RSP_NONE  0  /* not grounded -- answer NOTHING (see cm_response_shape) */
#define CM_RSP_ECHO  1  /* full-body 0x81 echo + the three sec 4(p) mutations */
#define CM_RSP_TOKEN 2  /* (txn,checksum) + OUR OWN node-parameter block only */
#define CM_RSP_DLM   3  /* verbatim echo + body[34]=0xf9, NO cat-0x01 mutations */

/*
 * cm_response_shape - decide how (and WHETHER) to answer a token-correlated
 * member request, from an ALLOWLIST of (category, opcode) pairs we have actually
 * grounded on the wire.
 *
 * WHY AN ALLOWLIST, AND WHY IT IS NOT OPTIONAL. The previous behaviour answered
 * EVERY token-correlated request, choosing only the SHAPE from the category and
 * defaulting to a full-body echo. The moment OVMX got the cluster-wide relay
 * working, the two non-coordinator members opened their own transactions with us
 * carrying opcodes we had never observed (0x12, 0x0f, 0x08, 0x00). We echoed all
 * of them, and CRASHED TWO REAL VAXES: VAX3 took INCONSTATE (Inconsistent I/O
 * data base) and VAX1 took INVEXCEPTN (exception above ASTDEL / on the interrupt
 * stack). A request body of this class carries the PEER'S OWN live Con.IDs and
 * cluster id; echoing it reflects that peer's own I/O structures back at it.
 * This is the identical failure a previous session hit by generalising the
 * category-0x01 echo to category 0x06.
 *
 * That is project Rule 10: never invent a handler for a condition we have not
 * grounded. A response we cannot justify from the reference is not a guess with
 * a small downside -- it is a fatal bugcheck on someone else's machine. When we
 * do not know, we say NOTHING and log loudly, and the gap becomes work.
 *
 * NOTE the honest risk of silence: a joiner that fails to answer something the
 * coordinator gates on can strand a cluster-wide transition, which times out and
 * drops healthy members (spec 4p). Silence is the SAFER failure -- recoverable,
 * and it leaves a log line naming exactly what we must ground next -- but it is
 * not free, and an unanswered op here is a bug to close, not a resting state.
 */
static int cm_response_shape(uint8_t category, uint8_t opcode)
{
    switch (category) {
    case SCS_MEMBER_CAT_CONFIG:
        /* GROUNDED. 0x03/0x05/0x09 from 6/6 responses over 5 captures and 3
         * responder nodes (spec 4p). 0x0f/0x08/0x12 from real VAX1<->VAX2 pairs
         * running this identical dialogue inside the crash capture
         * (f1367->f1368, f1372->f1373, f404->f406) -- 0x0f and 0x08 do not
         * occur as cat 0x01 ANYWHERE in the 17k-frame reference, so those pairs
         * are the only grounding that exists for them. */
        if (opcode == SCS_MEMBER_OP_COMMIT ||   /* 0x03 membership commit     */
            opcode == SCS_MEMBER_OP_LOCKRB ||   /* 0x05 lock/resource rebuild */
            opcode == SCS_MEMBER_OP_XITION ||   /* 0x09 class-0x02 open       */
            opcode == SCS_MEMBER_OP_RELAY  ||   /* 0x12 coordinator's relay   */
            opcode == SCS_MEMBER_OP_0F     ||   /* 0x0f class-0x03 extra step */
            opcode == SCS_MEMBER_OP_08     ||   /* 0x08 class-0x03 open       */
            /* vms-e4b: 0x0d is the class-0x04 open -- the message a node sends
             * when it is LEAVING of its own accord. GROUNDED 3/3 request/response
             * pairs from two captures and two different initiators, answered with
             * the identical echo recipe in ~0.5 ms. It was missing here, and a
             * cluster whose member departs gracefully would have met silence.
             *
             * ONLY in category 0x01. The cat-0x02 op 0x0d is the DLM rebuild
             * record and takes a completely different response (CM_RSP_DLM);
             * that split is why this allowlist is keyed per category. */
            opcode == SCS_MEMBER_OP_DEPART) {   /* 0x0d class-0x04 open       */
            return CM_RSP_ECHO;
        }
        return CM_RSP_NONE;
    case SCS_MEMBER_CAT_DLM:
        /* GROUNDED to an unusual degree. op 0x0d is the ONLY cat-0x02 opcode
         * that occurs during a join (216/216 in the reference), and its response
         * is a VERBATIM echo plus body[34]=0xf9 -- WITHOUT the cat-0x01
         * body[18]/body[55] mutations, which land inside the L1 region and the
         * lock RESOURCE NAME respectively. The recipe reconstructs 1367/1367
         * real responses byte-for-byte across four responder nodes and two
         * captures. See scs_member_build_dlm_response().
         *
         * The earlier reasoning -- "echoing a rebuild record asserts lock state
         * a joiner does not have" -- was WRONG, and it is worth saying so here
         * because it sounded right for three sessions. The echo returns the
         * COORDINATOR'S OWN record with a result code; it claims nothing about
         * our locks, which is exactly why a lock-less joiner can answer all 216.
         * What killed VAX1 and VAX3 was CORRUPTING the resource name
         * ("CACHE$cmSYSDSK1" -> "CACHE$c\0SYSDSK1"), not echoing it.
         *
         * Every other cat-0x02 opcode stays refused: none occurs during a join,
         * so none is grounded, and op 0x01/0x07/0x15 are known to use a
         * DIFFERENT result code (0xfa) -- exactly the near-miss that makes a
         * generalised handler fatal. */
        if (opcode == SCS_MEMBER_OP_DLM_REBUILD) {
            return CM_RSP_DLM;
        }
        /* op 0x01 (and its 3x-higher-volume sibling op 0x12): SILENCE IS THE
         * GROUNDED ANSWER HERE, not a gap to be closed. Do not "finish" this.
         *
         * A real responder's 0x82/0x01 reply is NOT derivable from the request:
         * it echoes body[0:20] and body[56:132] but REWRITES the 36-byte window
         * body[20:56] from its own lock-manager state. Mechanically tested over
         * 17218 real request/response pairs in two captures -- the op-0x0d
         * recipe reconstructs 0 of them, and no recipe short of "have a lock
         * database" reconstructs more than 37%. body[28:32] of the request is
         * the SENDER'S lock handle and body[112:116] is address-shaped sender
         * state; echoing either reflects a peer's live lock-database pointer,
         * which is the INCONSTATE/LOCKMGRERR failure mode exactly.
         *
         * And refusing costs nothing: in the run where OVMX reached MEMBER, one
         * arrived 47.8 s after membership, OVMX answered only the SCS credit,
         * VAX1 NEVER retransmitted it, and OVMX was not dropped -- the opposite
         * of op 0x0d, which retransmits 3x and freezes the barrier.
         *
         * OVMX holds no locks. The honest answer to a lock request is nothing.
         * Revisit only when OVMX has a real lock manager to answer FROM. */
        return CM_RSP_NONE;
    case SCS_MEMBER_CAT_MEMBERSHIP:
        /* Closes the transaction. Token + our OWN parameter block, never an
         * echo -- echoing this is what bugchecked VAX1 previously. */
        return CM_RSP_TOKEN;
    default:
        return CM_RSP_NONE;
    }
}

/*
 * vms-e81: OVMX's own MEMBER-STATE, as advertised in cat 0x01 op 0x01.
 *
 * While OVMX is joining it correctly emits the JOINER form of that message --
 * that is what it is. The defect was that it never stopped: as a sitting MEMBER
 * its op 0x01 stayed 131 of 132 bytes identical to what a node not in any
 * cluster sends. A newcomer asks every member "what cluster are you in?" and
 * OVMX answered "none, 0 members, admitted 1-JAN-2001" while the real members
 * described a 3-node cluster whose last transition was OVMX's own admission.
 * The newcomer could not close its view and never asked to join, for 678 s.
 *
 * RULE 10: `formed` and `last_transition` are CLUSTER-WIDE FACTS. They are
 * COPIED verbatim out of a member's own op 0x01 and never computed here. If we
 * have not heard them we stay in the joiner form, which is at least honest.
 */
static struct {
    int      known;           /* have we copied a member's op 0x01 yet? */
    int      admitted;        /* our own barrier completed -> we are a member */
    /* vms-197 (spec §4(O.38)): the AUTHENTIC cluster-admission latch, set when
     * OVMX RECEIVES the coordinator's op 0x06 MEMBERSHIP burst (the post-commit
     * membership publication). Distinct from `admitted` above, which latches off
     * OUR OWN barrier completion (all 12 op-0x0c steps) -- a DIFFERENT event that
     * RACES and can stay incomplete on a genuine member-oracle WIN (§4(O.38) r3:
     * only 4/12 barrier steps, so `admitted`/XITDONE stayed 0 while both members
     * reported CLUSTER_NODES 2->3). The membership burst is 1:1 with the oracle
     * (present on every WIN, absent on the LOSE), so THIS is the honest
     * self-report of admission. Receive-only: no wire change. */
    int      membership_committed;
    uint16_t member_count;
    uint64_t formed;
    uint64_t last_transition;
    uint64_t own_admission;
    /* vms-2f3: the FOUNDING NODE's SCSSYSTEMID -- SDA's `Found Node SYSID` in
     * the Cluster Block. Not carried in any field a member sends us under its
     * own name, but IDENTIFIABLE from what they do send: the founding node is
     * the member whose own admission time (op 0x01 body[64:72], SDA's CSB
     * `Ref. time`) equals the cluster's founding time (body[28:36], SDA's CLUB
     * `Founding Time`) -- because founding the cluster IS its admission.
     * Verified in d94-r1B: of three members, exactly VAX1/1025 matches, and
     * SDA on both VAX1 and VAX3 reports `Found Node SYSID 000000000401` = 1025.
     * Learned, never computed (Rule 10). */
    uint16_t founding_sysid;
} ovmx_cluster;

/*
 * vms-7a9: THE CONNECTION-MANAGER QUORUM MODEL. The connection manager consumes
 * each peer's advertised VOTES (grounded at the 190-byte VC body[22:24], spec
 * sec 4j) and OVMX's own non-voting contribution, and at every state transition
 * recomputes New CEVOTES = max{EXPECTED_VOTES; SUM VOTES; Old CEVOTES} and
 * QUORUM = (New CEVOTES + 2)/2 (VMScluster Systems sec 2.3.5; CM transcript
 * ch7-part01 pp. 7-5..7-7). scs_quorum.c holds the pure arithmetic + gate; this
 * is the live model instance the daemon feeds off the wire.
 *
 * ENFORCEMENT SCOPE. The gate (quorum-lost -> block-and-wait, no reconfigure --
 * vms-2d6) is COMPUTED and EXPOSED here; wiring it to suspend live I/O against a
 * real peer is admission-gated and lab-bracketed at 0.4 (this item is the
 * computation + consume + advertise, unit-proven). EXPECTED_VOTES is an RE gap
 * on the wire (held at 1 in every capture), so the model seeds each peer's
 * EXPECTED_VOTES from its advertised VOTES until a wire contrast grounds it.
 */
static struct scs_quorum cm_quorum;
static int cm_quorum_inited;
static int cm_quorum_last_present = -1; /* -1 = unknown; else last gate result */

/*
 * cm_quorum_note_peer_votes - fold a peer's just-parsed op-0x01 VOTES into the
 * quorum model, keyed by the peer's stable VMS$VAXcluster Con.ID, recompute, and
 * log when the quorum state (present<->lost) changes. Called only for a genuine
 * cat-0x01 op-0x01 cluster-parameters frame (view.has_votes == 1).
 */
static void cm_quorum_note_peer_votes(uint32_t peer_key, uint16_t peer_votes)
{
    if (!cm_quorum_inited) {
        scs_quorum_init(&cm_quorum);
        cm_quorum_inited = 1;
        /* OVMX's own contribution: non-voting (design sec 8), keyed on its own
         * SCSSYSTEMID so it can never break VAX quorum. */
        scs_quorum_set_member(&cm_quorum, resolve_scssystemid(),
                              SCS_MEMBER_VOTES_NONVOTING, 1, 1);
    }
    /* EXPECTED_VOTES: seed from advertised VOTES (RE gap -- see above). */
    scs_quorum_set_member(&cm_quorum, peer_key, peer_votes, peer_votes, 1);
    scs_quorum_recompute(&cm_quorum);

    int present = scs_quorum_present(&cm_quorum);
    if (present != cm_quorum_last_present) {
        cm_quorum_last_present = present;
        log_ts(stdout);
        printf(" SCSD-I-QUORUM, CEVOTES=%u QUORUM=%u present_votes=%u -> %s"
               " (peer 0x%08X votes=%u)\n",
               (unsigned)cm_quorum.cevotes, (unsigned)cm_quorum.quorum,
               (unsigned)cm_quorum.present_votes,
               present ? "quorum PRESENT (cluster runs)"
                       : "quorum LOST (suspend + wait, no reconfigure)",
               (unsigned)peer_key, (unsigned)peer_votes);
        fflush(stdout);
    }
}

/*
 * vms-2f3: PRIOR-ADMISSION STATE -- the one thing a rebooted VAX has that a
 * restarted OVMX does not.
 *
 * A real VAX3 that is `kill -9`'d, crash-removed by the cluster (class 0x03)
 * and rebooted under an unchanged SCSNODE/SCSSYSTEMID rejoins in ~90 s
 * (captures/vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap), and its op 0x02
 * says "I have prior cluster state" -- it REMEMBERS having been in this cluster
 * across the reboot. OVMX restarts amnesiac and claims to be a first-timer
 * while three peers hold a CSB for it, and the coordinator aborts the
 * transition 1.2 ms after collecting everyone's lock-rebuild echo.
 *
 * Persisted beside the SYSGEN store, because that store IS the identity
 * (SCSNODE + SCSSYSTEMID) this claim is about -- a different store is a
 * different node and must not inherit it.
 *
 * DELIBERATE SCOPE, and its relation to vms-e6c. e6c says: do not keep claiming
 * MEMBERSHIP you have lost. This keeps something strictly weaker and different
 * -- the fact that we were ONCE admitted to the cluster with this founding
 * time. We never replay a member count, a transition or a CSID from it, and it
 * gates exactly one boolean plus two values we re-learn live off the wire on
 * every run. If the cluster we meet has a different founding time, this is a
 * different cluster and the state does not apply.
 */
static struct {
    int      valid;
    uint64_t formed;          /* the cluster we were admitted to */
    uint16_t founding_sysid;  /* its founding node, as we knew it then */
    unsigned generation;      /* vms-e15: how many times this identity has been
                               * admitted. 1 on the first admission; the op 0x02
                               * REJOIN form carries prior+1 at body[36:40]. */
    unsigned clean;           /* vms-2f3: 1 iff the prior run recorded a CLEAN
                               * shutdown (scsd_shutdown_teardown rewrote the
                               * sidecar clean=1 AFTER its port-level last gasp).
                               * 0 (the on-admission value, or an old sidecar with
                               * no field) means the prior incarnation CRASHED
                               * without announcing its departure, so the peer may
                               * still be holding the corpse CSB in reconnect. */
} ovmx_prior;

/* vms-e15: the membership generation THIS run is admitted as -- 1 for a first
 * join, prior.generation+1 (floor 2) for a rejoin. Persisted on admission so a
 * restarted OVMX increments across boots, as a real VAX's incarnation ordinal
 * does. Only its non-zero presence on a rejoin is grounded (see
 * scs_member_build_config); the value is an OVMX ordinal, not a VMS byte. */
static unsigned ovmx_run_generation = 1;

/*
 * Where the prior-admission record lives: "<sysgen store>.cluster". Keyed on
 * the store because the store IS the identity the record is about -- a
 * different store is a different node and must not inherit it. With no named
 * store there is no stable identity to key on and no record is kept: an
 * unconfigured SCSD is a first-timer every time, which is at least honest.
 */
static int prior_admission_path(char *out, size_t out_len)
{
    const char *named = getenv("OVMX_SYSGEN_PATH");
    if (named == NULL || named[0] == '\0') {
        return -1;
    }
    int n = snprintf(out, out_len, "%s.cluster", named);
    return (n > 0 && (size_t)n < out_len) ? 0 : -1;
}

static void prior_admission_load(void)
{
    char path[1024];
    memset(&ovmx_prior, 0, sizeof(ovmx_prior));
    if (prior_admission_path(path, sizeof(path)) != 0) {
        return;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    unsigned long long formed = 0;
    unsigned sysid = 0;
    unsigned generation = 0;
    unsigned clean = 0;
    if (fscanf(f, "formed=%llx founding_sysid=%u", &formed, &sysid) == 2 &&
        formed != 0) {
        ovmx_prior.valid = 1;
        ovmx_prior.formed = (uint64_t)formed;
        ovmx_prior.founding_sysid = (uint16_t)sysid;
        /* generation and clean are later fields, written in that order: an old
         * sidecar lacks both (fscanf misses, they stay 0 -- generation then
         * floors to 2 on a rejoin, and clean=0 conservatively treats an old
         * sidecar as a possible crash, which only costs a harmless corpse gasp
         * the peer has no CSB to act on). */
        if (fscanf(f, " generation=%u", &generation) == 1) {
            ovmx_prior.generation = generation;
            if (fscanf(f, " clean=%u", &clean) == 1) {
                ovmx_prior.clean = clean;
            }
        }
    }
    fclose(f);
    if (ovmx_prior.valid) {
        log_ts(stdout);
        printf(" SCSD-I-PRIORCLU, this identity has been admitted before:"
               " cluster formed=0x%016llx, founding node SCSSYSTEMID %u,"
               " prior run shut down %s (%s). If the cluster we meet carries that"
               " founding time, our op 0x02 will take the REJOIN form%s\n",
               (unsigned long long)ovmx_prior.formed,
               (unsigned)ovmx_prior.founding_sysid,
               ovmx_prior.clean ? "CLEANLY" : "with NO clean-leave (CRASH)", path,
               ovmx_prior.clean ? "" :
               " -- and we will first emit the crashed incarnation's last gasp");
        fflush(stdout);
    }
}

static void prior_admission_write(unsigned clean)
{
    char path[1024];
    if (prior_admission_path(path, sizeof(path)) != 0 ||
        ovmx_cluster.formed == 0) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fprintf(f, "formed=%016llx founding_sysid=%u generation=%u clean=%u\n",
            (unsigned long long)ovmx_cluster.formed,
            (unsigned)ovmx_cluster.founding_sysid,
            ovmx_run_generation, clean);
    fclose(f);
    log_ts(stdout);
    printf(" SCSD-I-PRIORCLU, recorded our admission to cluster"
           " formed=0x%016llx (founding node %u, clean=%u) in %s -- a rebooted"
           " VAX carries this across a crash and a restarted OVMX must too\n",
           (unsigned long long)ovmx_cluster.formed,
           (unsigned)ovmx_cluster.founding_sysid, clean, path);
    fflush(stdout);
}

/*
 * prior_admission_save - vms-2f3: called at ADMISSION. The record is written
 * DIRTY (clean=0): we are now a live incarnation and if we crash before a clean
 * teardown the peer will still hold our CSB, so our NEXT boot must announce this
 * incarnation's departure. scsd_shutdown_teardown() rewrites clean=1 only after
 * a genuine clean-leave last gasp has gone out.
 */
static void prior_admission_save(void)
{
    prior_admission_write(0);
}

/*
 * prior_admission_mark_clean - vms-2f3: called at CLEAN shutdown, AFTER the
 * port-level last gasp has already announced our departure. Stamps clean=1 so
 * the next boot does NOT emit a corpse last gasp -- the peer removed us at
 * departure and there is nothing left to retract.
 */
static void prior_admission_mark_clean(void)
{
    prior_admission_write(1);
}

/*
 * ovmx_rejoin_cleanleave - vms-ab1 (spec 4(O.29)): THE THREAD-CLOSER SWITCH.
 *
 * DEFINITIVE ROOT CAUSE (spec 4(O.28), design-map sec 12): OVMX's whole
 * reconnect/readmit/rejoin apparatus was the bug. On return the departed node's
 * OWN per-SCSSYSTEMID CSB sits on the coordinator in State 03 reconnect -> 09
 * wait, Flags 02040001 long_break, CSID 0; OVMX's rejoin-form return drives op
 * 0x02/readmit onto that still-open reconnect, which can NEVER complete (OVMX
 * returns a fresh incarnation/Con.ID), so it perpetually re-arms the long_break
 * wait and the CSB is never removed, never re-proposed (sameid358 R/J,
 * cq358). Davis pp. 7-24/7-25 are categorical: a rejoin has NO distinct
 * transaction -- the old CSB is deallocated and a new one is built "just as if
 * joining for the first time", new CSID. There is NO separate rejoin protocol.
 *
 * THE FIX, in two parts, both gated here (DEFAULT ON):
 *   (1) DEPARTURE emits a clean CM self-departure (the class-0x04 op 0x0d
 *       transition-open OVMX already PARSES inbound; spec 4(O.28), Davis p.
 *       7-29) so the coordinator REMOVES OVMX's prior incarnation's CSB
 *       immediately instead of holding it in reconnect/long_break. See
 *       scsd_emit_clean_departure() at shutdown.
 *   (2) RETURN abandons the reconnect/readmit rejoin path entirely and rejoins
 *       as a PLAIN FRESH FIRST-JOIN: cm_rejoin_target_mode() returns 0 (own
 *       outbound VMS$VAXcluster VC, the proven first-join topology) and
 *       cm_apply_rejoin_form() is a no-op (plain first-timer op 0x02, no rejoin
 *       fields). Because every rejoin sub-lever (lean-VC, credit-first,
 *       lean-early-hold, ackhold, the proactive CMREADMIT burst) chains through
 *       cm_rejoin_target_mode(), neutralising it disables the whole apparatus.
 *
 * KILL SWITCH: OVMX_REJOIN_CLEANLEAVE=0 restores the pre-vms-ab1 behaviour --
 * the reconnect/readmit apparatus + rejoin-form op 0x02 + no last-gasp -- so the
 * pre-fix stall stays reproducible and the change stays bisectable (guardrail
 * 23). Read fresh every call.
 */
static int ovmx_rejoin_cleanleave(void)
{
    const char *off = getenv("OVMX_REJOIN_CLEANLEAVE");
    if (off != NULL && off[0] == '0' && off[1] == '\0') {
        return 0;
    }
    return 1;
}

/*
 * ovmx_join_all_members - vms-694 (spec 4(O.35)): THE TESTED-AND-REFUTED
 * RETURN-SIDE TOTAL-CONNECTIVITY HYPOTHESIS. Default OFF. This is NOT a fix --
 * it is the bisectable NEGATIVE CONTROL that GROUNDS the 4(O.35) verdict that a
 * deterministic return-side reclaim-engagement fix is NOT OVMX-forceable.
 *
 * WHAT 4(O.35) ISOLATED (single-factor first-join-vs-return bracket on virgin
 * lab-2, VAX F$GETSYI("CLUSTER_NODES") + DCL SHOW CLUSTER oracles):
 *   - The member reclaims its departed CSB PROMPTLY and deterministically: SHOW
 *     CLUSTER for the returner goes BRK_NON -> NEW within ~4 s (a fresh CSB, per
 *     Davis pp. 7-24/7-25). The member is NOT "not engaging"; it reclaims.
 *   - A return is admitted (CLUSTER_NODES 2->3) IFF OVMX's VMS$VAXcluster joiner
 *     VC reaches OPEN with BOTH members -- the Rule of Total Connectivity (Davis
 *     p. 7-39). WIN (Release): both members open their OWN SCS$DIRECTORY to OVMX
 *     and both joiner VCs reach OPEN via the member's RCV_ACCEPT_REQ, then the
 *     coordinator runs op 0x03 COMMIT -> op 0x05 -> op 0x06 MEMB. LOSE (Debug):
 *     only ONE member opens SCS$DIRECTORY to OVMX and only one VC reaches OPEN;
 *     the member's NEW CSB stalls, CLUSTER_NODES stays 2.
 *   - THE COMPLETING STEP IS MEMBER-DRIVEN. A joiner VC reaches OPEN only when
 *     the member sends RCV_ACCEPT_REQ (its SYSAP accepts) -- which the member
 *     does only after IT has opened its own SCS$DIRECTORY to OVMX, i.e. once its
 *     CM has re-engaged the returner. On a slow return only one member's CM does
 *     this; the second's non-engagement is its residual-CSB reclaim state.
 *
 * WHY THIS SWITCH REFUTES THE OBVIOUS FIX. If total connectivity were merely a
 * matter of OVMX opening a joiner CONNECT-REQ to both members, forcing it would
 * win. It does NOT. Engaged, this opens the joiner VC to EVERY start_acked member
 * off OVMX's own clock. Measured (o35proof, patched Debug daemon, 10 consecutive
 * returns each from a confirmed CLUSTER_NODES=2): 0/10 wins. The pre-emptive
 * CONNECT-REQ gets a transport CONNECT_RESPONSE (VC -> CONNECT ACK) but the
 * member NEVER sends ACCEPT_REQUEST, because ACCEPT_REQUEST is the member CM's
 * decision, gated on its reclaim -- so both VCs stall in CONNECT ACK and the
 * return reliably LOSES (worse than the unpatched daemon, which at least lands
 * the one member-prompted VC). OVMX cannot force the member's ACCEPT_REQUEST.
 *
 * ENGAGE: OVMX_JOIN_ALL_MEMBERS=1 (for reproducing the refutation only; never a
 * production fix). Read fresh every call. No-op under OVMX_PURE_SERVER /
 * OVMX_NO_OWN_VC / the join sequencer. NOT the op-0x0d crash class (spec
 * 4(O.29)) -- it is the plain joiner CONNECT-REQ OVMX already emits.
 */
static int ovmx_join_all_members(void)
{
    const char *on = getenv("OVMX_JOIN_ALL_MEMBERS");
    return (on != NULL && on[0] == '1' && on[1] == '\0');
}

/*
 * vms-2f3: decide whether this op 0x02 takes the REJOIN form, and fill it.
 *
 * Every condition here is a fact we can check, not a preference:
 *   - we have a prior-admission record for THIS identity, and
 *   - we have heard a member's op 0x01 this run, and
 *   - the cluster we are meeting has the SAME founding time we were admitted
 *     to. A different founding time is a different cluster and we are a
 *     first-timer to it, no matter what we remember.
 *   - and we know who founded it -- learned live this run if a member told us,
 *     falling back to the value we recorded at our own admission.
 *
 * OVMX_REJOIN_FORM=0 forces the first-join form, so the refusal stays
 * reproducible and the change stays bisectable.
 */
static void cm_apply_rejoin_form(struct scs_member_params *mp)
{
    static int announced;
    /* vms-ab1 (spec 4(O.29)): under the clean-leave default the return is a
     * PLAIN FRESH FIRST-JOIN, so op 0x02 carries NO rejoin fields at all. */
    if (ovmx_rejoin_cleanleave()) {
        return;
    }
    const char *off = getenv("OVMX_REJOIN_FORM");
    if (off != NULL && off[0] == '0') {
        return;
    }
    if (!ovmx_prior.valid || !ovmx_cluster.known || ovmx_cluster.formed == 0 ||
        ovmx_cluster.formed != ovmx_prior.formed) {
        return;
    }
    uint16_t fs = ovmx_cluster.founding_sysid;
    if (fs == 0) {
        fs = ovmx_prior.founding_sysid;
    }
    if (fs == 0) {
        return;
    }
    mp->rejoin = 1;
    mp->founding_sysid = fs;
    mp->cluster_formed = ovmx_cluster.formed;

    /* vms-e15: body[36:40] -- the membership generation ordinal. A real rejoin
     * carries a non-zero value here (9/2/2/3 across four specimens); OVMX left
     * it zero. OVMX now emits it for WIRE FIDELITY. This run's generation is
     * prior+1 (floor 2 -- the SECOND membership of an identity we were admitted
     * to once). The value is an OVMX ordinal, not member-validated; only its
     * non-zero presence on a rejoin is grounded.
     *
     * ⚠ This is NOT the readmission fix (spec sec 4(O.10), proven live): with
     * body[36:40]=2 the member still declines to reciprocate on the joiner VC and
     * XITDONE stays 0. The real blocker is downstream and is deferred to vms-694.
     *
     * OVMX_REJOIN_GEN=0 forces body[36:40] back to zero (the historical form) so
     * the change stays bisectable and the kill-switch gates exactly its own field. */
    {
        const char *gen_off = getenv("OVMX_REJOIN_GEN");
        if (gen_off != NULL && gen_off[0] == '0') {
            ovmx_run_generation = 0;
        } else {
            unsigned base = (ovmx_prior.generation >= 1) ? ovmx_prior.generation : 1;
            ovmx_run_generation = base + 1;
        }
        mp->rejoin_generation = ovmx_run_generation;
    }

    if (!announced) {
        announced = 1;
        log_ts(stdout);
        printf(" SCSD-I-CMREJOIN, op 0x02 takes the REJOIN form: prior"
               " state=1, founding node %u, founding time 0x%016llx,"
               " membership generation %u (body[36:40]). A real VAX that"
               " crash-rejoins carries a non-zero generation here"
               " (crash-rejoin #1297=9) and OVMX now matches it (wire fidelity)."
               " NOTE (vms-e15): this alone does NOT complete readmission -- the"
               " member still declines to reciprocate on the joiner VC; see"
               " spec sec 4(O.10), blocker deferred to vms-694\n",
               (unsigned)fs, (unsigned long long)ovmx_cluster.formed,
               ovmx_run_generation);
        fflush(stdout);
    }
}

/*
 * vms-4838: is this a REJOIN that must run CM readmission on the MEMBER-INITIATED
 * VMS$VAXcluster connection (rejoiner = Figure-2-14 TARGET), rather than on
 * OVMX's own outbound joiner VC?
 *
 * GROUNDED (spec sec 4(O.11), SUCCESS oracle vax3-class03-crash-REJOIN-SUCCESS):
 * a crash-rejoiner NEVER opens its own outbound VMS$VAXcluster connect; BOTH
 * established members open the VMS$VAXcluster CONNECT_REQ *to* the returning
 * node (f1202, f1248) and the rejoiner answers each as the TARGET (echo f1249 +
 * accept-req f1250, lcid 45b80009), then drives the ENTIRE readmission
 * choreography -- op 0x02 (f1297) -> member cat 0x04 op 0x04 (f1299) -> op 0x03
 * commit (f1303) -> op 0x05 lock-rebuild (f1306) -> op 0x06 barrier (f1320) --
 * on that member-initiated connection. OVMX's default sequencer ALWAYS opens its
 * own outbound VMS$VAXcluster connect at step 7/8 and drives op 0x02 there:
 * correct for a FIRST join (the member ACCEPTs it -> OPEN -> XITDONE=1), WRONG
 * for a rejoin (the member refuses/ignores the outbound while its own inbound
 * connect reaches OPEN and is left idle -> op 0x02 never lands where the member
 * is listening -> XITDONE=0).
 *
 * The runtime discriminator is the PRIORCLU fact: ovmx_prior.valid means this
 * identity holds a persisted prior-admission record (SCSD-I-PRIORCLU) -- i.e. a
 * member legitimately still holds a residual CDT for it and will re-open the
 * VMS$VAXcluster connection to us. This is a checkable fact, not a preference.
 *
 * Kill-switch OVMX_REJOIN_TARGET=0 forces the historical first-join topology
 * (drive on OVMX's own outbound VC) so the pre-fix stall stays reproducible and
 * the change stays bisectable (guardrail 23). OVMX_NO_OWN_VC keeps forcing the
 * member-initiated topology ON unconditionally, as before (its manual override).
 */
static int cm_rejoin_target_mode(void)
{
    if (getenv("OVMX_NO_OWN_VC") != NULL) {
        return 1;   /* manual override, unchanged (kept for bisection) */
    }
    /* vms-ab1 (spec 4(O.29)): the clean-leave default ABANDONS the
     * reconnect/readmit rejoin topology and returns on OVMX's OWN outbound
     * VMS$VAXcluster VC -- the proven first-join path. This chains through every
     * rejoin sub-lever (rejoin_credit_first_enabled, cm_rejoin_lean_vc, the
     * proactive CMREADMIT burst, the own-VC-suppression branch), disabling the
     * whole apparatus at one point. OVMX_REJOIN_CLEANLEAVE=0 restores the
     * ovmx_prior.valid-driven rejoin path below. */
    if (ovmx_rejoin_cleanleave()) {
        return 0;
    }
    const char *off = getenv("OVMX_REJOIN_TARGET");
    if (off != NULL && off[0] == '0' && off[1] == '\0') {
        return 0;
    }
    return ovmx_prior.valid ? 1 : 0;
}

/*
 * cm_rejoin_lean_vc - vms-9af: on a rejoin, keep the COORDINATOR member's
 * virtual circuit LEAN by SUPPRESSING OVMX's OWN SCS$DIRECTORY + MSCP$DISK
 * CLIENT connects to the coordinator (the peer whose VMS$VAXcluster VC carries
 * the op 0x02 readmission).
 *
 * GROUNDED (spec sec 4(O.15), vms-e15, SUCCESS oracle + live lab-2 bracket).
 * send_seq is per-VC (sec 4(O.14)); every connection OVMX multiplexes onto the
 * coordinator shares that one VC's sequence. When OVMX opens its OWN
 * SCS$DIRECTORY + MSCP$DISK client connections to the coordinator and runs
 * directory + disk discovery on them, ~19 of its own sequenced messages precede
 * the readmission, so op 0x02 lands at ss=21 -- BEHIND the member's recv_ack
 * ceiling of 19 -- and is never delivered; the member breaks the VC, XITDONE=0.
 * The SUCCESS oracle rejoiner keeps the coordinator's VC lean: it opens its own
 * dir/MSCP client connections to a NON-coordinator member (sec 4(O.11) census)
 * and merely ANSWERS the coordinator's inbound connects, so op 0x02 reaches the
 * coordinator at ss=14, under the ceiling, and completes.
 *
 * Kill-switch OVMX_REJOIN_LEAN_VC=0 disables this (restores opening the client
 * half to EVERY member, coordinator included -- the pre-fix stall). Gated on
 * cm_rejoin_target_mode() so it fires only on a detected rejoin.
 */
static int cm_rejoin_lean_vc(void)
{
    if (!cm_rejoin_target_mode()) {
        return 0;
    }
    const char *off = getenv("OVMX_REJOIN_LEAN_VC");
    if (off != NULL && off[0] == '0' && off[1] == '\0') {
        return 0;
    }
    return 1;
}

/*
 * rejoin_credit_first_enabled - vms-46f (spec 4(O.17)): THE root-cause fix for
 * the rejoin stall. On a rejoin the coordinator returns cm_responses=0 because
 * it is SEND-CREDIT-STARVED (Davis pp. 2-43/2-45): OVMX never returns Pending
 * Receive Credit to it, so its Send Credit to OVMX is never replenished and it
 * cannot transmit its op 0x04 reciprocation (Credit Wait). The SUCCESS oracle
 * INITIATES its own op-6 special-credit request on the coordinator's
 * SCS$DIRECTORY connection BEFORE op 0x02 (f1261 ss=13, THEN op 0x02 f1297
 * ss=14); OVMX deferred its op-6 to ss=14/15 -- too late. This switch makes
 * OVMX send the op-6 credit round AHEAD of the deferred op 0x02 on a rejoin.
 *
 * Kill-switch OVMX_REJOIN_CREDIT_FIRST=0 restores the pre-fix order (op 0x02
 * first, credit-starved coordinator). Gated on cm_rejoin_target_mode() so it
 * fires only on a detected rejoin; the first-join burst is unchanged.
 */
static int rejoin_credit_first_enabled(void)
{
    if (!cm_rejoin_target_mode()) {
        return 0;
    }
    const char *off = getenv("OVMX_REJOIN_CREDIT_FIRST");
    if (off != NULL && off[0] == '0' && off[1] == '\0') {
        return 0;
    }
    return 1;
}

/*
 * cm_peer_is_coordinator - vms-9af: is this peer the "coordinator" whose
 * VMS$VAXcluster VC carries the op 0x02 readmission -- the VC that must stay
 * lean on a rejoin?
 *
 * Decided the SAME way the deferred op 0x02 recipient is (cm_pick_coordinator /
 * OVMX_CFG2_PEER), but usable BEFORE config exchange: the own-dir client connect
 * is join step 1/8, which fires right after the 0x41 START handshake, before any
 * peer has cfg_sent. peer_node_number() reads the SCA logical address (present
 * post-START), so the highest-node-number rule -- cm_pick_coordinator's rule
 * MINUS the cfg_sent filter -- is evaluable here and stable (node numbers do not
 * change). OVMX_CFG2_PEER, when set, names the coordinator's node number
 * deterministically and wins.
 *
 * HOW a real joiner identifies the coordinator is NOT grounded (see
 * cm_pick_coordinator's header): no wire-visible coordinator flag was found.
 * This is therefore an OVMX SELECTION choice -- wire-visible (op 0x02's ss moves)
 * and kill-switched -- not presented as VMS-authentic.
 *
 * TIMING CAVEAT (live vaxlab-0): the two members complete their 0x41 START in a
 * NON-deterministic order, and this predicate is evaluated at each peer's own-dir
 * INITIATION -- so the strict-max-plus-lower-peer heuristic below only suppresses
 * the coordinator when the NON-coordinator (lower) member happened to appear
 * first. When the coordinator appears first, no lower peer is yet known, it is not
 * suppressed, and the result is the pre-fix bloat (NO regression, but the fix does
 * not engage). OVMX_CFG2_PEER removes the timing dependence entirely by naming the
 * coordinator's node number up front, and is the deterministic path for a bracket.
 */
static int cm_peer_is_coordinator(struct peer_state *tbl, struct peer_state *ps)
{
    if (ps == NULL || ps->pb == NULL) {
        return 0;
    }
    uint16_t nn = peer_node_number(ps);
    const char *force = getenv("OVMX_CFG2_PEER");
    if (force != NULL) {
        uint16_t f = (uint16_t)strtoul(force, NULL, 0);
        return (nn == f || (nn & 0x03ff) == f) ? 1 : 0;
    }
    /* This peer is the coordinator ONLY IF it is the STRICT MAXIMUM node number
     * AND at least one OTHER peer with a LOWER node number is already known.
     *
     * The "lower peer exists" guard is load-bearing, not cosmetic. The own-dir
     * client connect fires per peer as each completes its 0x41 START, at
     * different times. A bare "highest known" test suppressed the FIRST member to
     * appear (it was momentarily the highest known), then suppressed the SECOND
     * too once it appeared and became the highest -- leaving NO member for OVMX's
     * own dir/MSCP discovery, so the member never saw OVMX's MSCP$DISK connect and
     * never reciprocated (live vaxlab-0, both members LEANVC-suppressed,
     * XITDONE=0). The oracle keeps exactly ONE VC (the coordinator's) lean and
     * runs discovery against the OTHER member (spec 4(O.11) census). Requiring a
     * strictly-lower peer to be present means: the lower (non-coordinator) member
     * is never suppressed (nothing higher than it is required), and the higher
     * (coordinator) member is suppressed only once that non-coordinator exists to
     * carry discovery. OVMX_CFG2_PEER names the coordinator deterministically when
     * the appearance order cannot be relied on. */
    int lower_peer_seen = 0;
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        if (&tbl[i] == ps || tbl[i].pb == NULL) {
            continue;
        }
        uint16_t n2 = peer_node_number(&tbl[i]);
        if (n2 > nn) {
            return 0;              /* someone higher -> ps is not the coordinator */
        }
        if (n2 < nn) {
            lower_peer_seen = 1;   /* a non-coordinator member to run discovery against */
        }
    }
    return lower_peer_seen ? 1 : 0;
}

/*
 * cm_lean_vc_suppress_peer - vms-9af: should OVMX's OWN dir/MSCP client half be
 * suppressed for THIS peer right now? True iff coordinator-lean mode is active
 * (rejoin + kill-switch on) and this peer is the coordinator. Latches
 * ps->lean_vc_suppressed and logs SCSD-I-LEANVC once so the wire-visible
 * decision appears in the transcript exactly one time per peer.
 */
static int cm_lean_vc_suppress_peer(struct peer_state *tbl, struct peer_state *ps)
{
    if (!cm_rejoin_lean_vc() || !cm_peer_is_coordinator(tbl, ps)) {
        return 0;
    }
    if (!ps->lean_vc_suppressed) {
        ps->lean_vc_suppressed = 1;
        log_ts(stdout);
        printf(" SCSD-I-LEANVC, REJOIN: SUPPRESS OUR OWN SCS$DIRECTORY + MSCP$DISK"
               " client half to the COORDINATOR node %u -- keep its VC LEAN so op"
               " 0x02 rides at a low send_seq (spec 4(O.15)); disk discovery runs"
               " against a NON-coordinator member\n",
               (unsigned)(peer_node_number(ps) & 0x03ff));
        fflush(stdout);
    }
    return 1;
}

/*
 * cm_peer_coord_pending - vms-3aba (spec 4(O.27)): is this peer a coordinator
 * CANDIDATE whose coordinator status is NOT YET DECIDABLE? True iff it is the
 * highest node number known so far AND no strictly-lower peer has appeared yet,
 * so cm_peer_is_coordinator() returns 0 ONLY for want of a lower peer (not
 * because someone higher exists). A lower peer appearing would flip it to the
 * coordinator (suppress); the topology has simply not settled.
 *
 * This is the exact race the vms-9af TIMING CAVEAT (cm_peer_is_coordinator's
 * header) names: when the coordinator's 0x41 START arrives before the
 * non-coordinator's, own-dir initiation runs while this predicate is true, the
 * lean-VC suppression does NOT fire, and the coordinator's VC is bloated before
 * op 0x02 (spec 4(O.26)). When OVMX_CFG2_PEER names the coordinator up front the
 * status is already decided, so there is nothing pending.
 */
static int cm_peer_coord_pending(struct peer_state *tbl, struct peer_state *ps)
{
    if (ps == NULL || ps->pb == NULL) {
        return 0;
    }
    if (getenv("OVMX_CFG2_PEER") != NULL) {
        return 0;              /* coordinator is named deterministically -> decided */
    }
    uint16_t nn = peer_node_number(ps);
    int lower_peer_seen = 0;
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        if (&tbl[i] == ps || tbl[i].pb == NULL) {
            continue;
        }
        uint16_t n2 = peer_node_number(&tbl[i]);
        if (n2 > nn) {
            return 0;          /* someone higher -> ps is decidedly NOT the coordinator */
        }
        if (n2 < nn) {
            lower_peer_seen = 1;
        }
    }
    return lower_peer_seen ? 0 : 1;   /* top node, no lower peer yet -> PENDING */
}

/*
 * cm_rejoin_lean_early_hold - vms-3aba (spec 4(O.27)): should OVMX HOLD its own
 * dir/MSCP client half to THIS peer this tick, because it is a coordinator
 * candidate whose lean-VC decision cannot yet be made (a lower peer has not yet
 * appeared)?  This is what makes lean-VC / credit-first engage BEFORE op 0x02 on
 * the member-initiated connection (spec 4(O.26) fix): rather than initiate
 * own-dir on the highest-known node the instant it completes START -- and then
 * discover ~1.3 s later that it was the coordinator whose VC we just bloated --
 * we WAIT out a bounded settle window for the non-coordinator to appear, at
 * which point cm_lean_vc_suppress_peer() suppresses the coordinator permanently
 * and own-dir never rides its VC. op 0x02 then lands at a low send_seq, under
 * the coordinator's recv_ack ceiling of 18.
 *
 * Bounded by LEAN_COORD_SETTLE_MS so a genuinely lone existing member (no lower
 * peer will ever appear) is not starved of discovery forever -- past the window
 * we give up holding and initiate. Kill-switch OVMX_REJOIN_LEAN_EARLY=0 restores
 * the pre-fix behaviour (own-dir initiates the instant START completes, and
 * lean-VC engages too late on an appears-first coordinator). Gated on
 * cm_rejoin_lean_vc() so it fires only on a detected rejoin with lean-VC on; the
 * first-join path and the non-coordinator peer are untouched.
 */
static int cm_rejoin_lean_early_hold(struct peer_state *tbl, struct peer_state *ps,
                                     long now_ms)
{
    if (!cm_rejoin_lean_vc()) {
        return 0;
    }
    const char *off = getenv("OVMX_REJOIN_LEAN_EARLY");
    if (off != NULL && off[0] == '0' && off[1] == '\0') {
        return 0;             /* kill-switch = pre-fix late-firing behaviour */
    }
    if (!cm_peer_coord_pending(tbl, ps)) {
        return 0;             /* coordinator status already decidable -> nothing to hold */
    }
    if (ps->lean_hold_start_ms == 0) {
        ps->lean_hold_start_ms = now_ms;
        log_ts(stdout);
        printf(" SCSD-I-LEANHOLD, REJOIN: HOLD OUR OWN dir/MSCP client half to"
               " candidate coordinator node %u until the topology settles (a"
               " lower non-coordinator member appears) -- so lean-VC engages"
               " BEFORE op 0x02 and op 0x02 rides under the recv_ack ceiling"
               " (spec 4(O.26)/4(O.27))\n",
               (unsigned)(peer_node_number(ps) & 0x03ff));
        fflush(stdout);
    }
    if ((now_ms - ps->lean_hold_start_ms) >= (long)LEAN_COORD_SETTLE_MS) {
        return 0;             /* settle window elapsed -> liveness: initiate anyway */
    }
    return 1;                 /* HOLD: keep the candidate coordinator's VC lean */
}

/*
 * vms-71d: on a REJOIN, HOLD the standalone category-0x04 CM ack until OVMX's
 * own op 0x02 config has ridden the member-initiated connection.
 *
 * GROUNDED, SUCCESS oracle vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap
 * (spec sec 4(O.13)). On the member-initiated connection the crash-rejoiner VAX3
 * sends its add-member advertisement as THREE CONTIGUOUS SYSAP messages --
 * op 0x14 model (f1257, sms=1), op 0x01 params (f1258, sms=2), op 0x02 config
 * (f1298, sms=3, ams=2) -- and the coordinator reciprocates op 0x04 ~1 ms later
 * (f1299, ams=3). VAX3 emits NO standalone cat-0x04 in that window; its FIRST is
 * f1330 (sms=9), AFTER the reciprocation. The config's own ams field (=2) is
 * what acknowledges the member's model+params -- no separate ack frame is used.
 *
 * OVMX's poll-loop flush (and, in principle, the receive-path batch ack) instead
 * emitted a cat-0x04 op 0x00 ack BETWEEN params (sms=2) and the deferred op 0x02,
 * consuming sms=3 and pushing the op 0x02 to sms=4 -- a frame the reference never
 * sends there, and a config the coordinator did not treat as the one it
 * reciprocates to (live vaxlab-11 2026-08-10: op 0x02 sms=4, member silent,
 * XITDONE=0). Holding the standalone ack until the op 0x02 is on the wire makes
 * OVMX's stream model(1)->params(2)->config(3) exactly as the oracle does; the
 * op 0x02 already carries ams=sysap_recv (=2), so the acknowledgment is not lost,
 * only carried inline as the reference carries it. After the op 0x02 the hold
 * releases and normal ack flushing resumes to carry the member-driven commit /
 * lock-rebuild / barrier round (the reference's own post-reciprocation acks).
 *
 * Scoped to the rejoin readmission (cm_rejoin_target_mode() && !joiner_cfg2_sent)
 * so the working first-join path is byte-unchanged (guard 8). Wire-visible ->
 * kill-switch OVMX_REJOIN_ACKHOLD=0 restores the pre-fix behaviour (the standalone
 * ack fires before op 0x02, op 0x02 rides sms=4) so the change stays bisectable
 * and the stall stays reproducible (guardrail 23).
 */
static int rejoin_hold_standalone_ack(const struct peer_state *ps)
{
    if (!cm_rejoin_target_mode() || ps->joiner_cfg2_sent) {
        return 0;
    }
    const char *off = getenv("OVMX_REJOIN_ACKHOLD");
    if (off != NULL && off[0] == '0' && off[1] == '\0') {
        return 0; /* kill-switch: pre-fix behaviour */
    }
    return 1;
}

/*
 * vms-584: RE-LEARN the cluster-wide facts from the TRANSITION-OPEN.
 *
 * COPY-ONCE WAS THE WRONG MODEL, AND op 0x01 WAS THE WRONG SOURCE. Run by13
 * caught the defect live: OVMX copied member_count=2 from a peer BEFORE its own
 * admission, nothing ever re-taught it 3, and at frame 2941 OVMX advertised
 * "2 members" to VAX3 while VAX1 (frame 2869) and VAX2 had said "3" on the same
 * wire seconds earlier.
 *
 * A member's op 0x01 is only a point-in-time REPLY to a newcomer's query. It is
 * sent once per VC, and it may simply never arrive again before the next
 * transition -- which is exactly how our copy went stale. GROUNDED alternative
 * (26-capture census): the cat-0x01 TRANSITION-OPEN (op 0x09 add / 0x08 remove /
 * 0x0d depart) is the bundle. Every node sees it, member or bystander, on every
 * transition of every class, and it carries in one frame:
 *
 *   body[40:48]  the transition-time quadword -- the value that later shows up
 *                as last_transition in a member's op 0x01. Matched to the
 *                millisecond against OPCOM, and present 20 ms BEFORE OPCOM logs
 *                "completed", so the fact is available at open time.
 *   body[55]     the coordinator's MEMBERSHIP BITMAP. popcount == the
 *                post-transition member count, 54/54 opens, zero residuals.
 *
 * Same encoding as `formed` (VMS quadword, 1858 epoch, 100 ns ticks).
 * `formed` itself stays copy-once: it never changes in any capture.
 *
 * TWO DELIBERATE LIMITS. (1) We read the bitmap's POPCOUNT only. The bit-to-node
 * mapping is NOT grounded -- both observed transitions set a contiguous run, which
 * fits "bit k = CSID slot" and "bit k = join order" equally, and one byte cannot
 * be the whole field (body[52:55] and body[56:60] are zero in every specimen, so
 * the extent is undetermined). Counting bits needs no mapping; asserting the
 * bitmap would, so we never assert it. (2) Class 0x04 has no barrier, so its
 * facts apply at open; classes 0x02/0x03 apply when OUR barrier completes, which
 * is when the transition they describe is actually real.
 */
static void ovmx_cluster_relearn(uint16_t members, uint64_t last_transition,
                                 const char *when)
{
    if (members == 0) {
        /* A zero popcount would mean "a cluster with no members", which is not
         * a thing. Refuse it rather than advertise it -- staying stale is bad,
         * but advertising an impossible cluster is worse. */
        log_ts(stdout);
        printf(" SCSD-W-CLUSTATE, transition-open carried an empty membership"
               " bitmap -- REFUSING to re-learn a member count of 0 (%s)\n", when);
        fflush(stdout);
        return;
    }
    if (ovmx_cluster.member_count != members ||
        ovmx_cluster.last_transition != last_transition) {
        log_ts(stdout);
        printf(" SCSD-I-CLUSTATE, re-learned from the transition-open (%s):"
               " members %u -> %u, last_transition 0x%016llx -> 0x%016llx\n",
               when, (unsigned)ovmx_cluster.member_count, (unsigned)members,
               (unsigned long long)ovmx_cluster.last_transition,
               (unsigned long long)last_transition);
        fflush(stdout);
    }
    ovmx_cluster.known = 1;
    ovmx_cluster.member_count = members;
    ovmx_cluster.last_transition = last_transition;
}

/* vms-578 INTEGRATION: worktree-760's send_frame_to() is GONE. It was a raw
 * sendto() wrapper, i.e. a second transmit primitive alongside send_frame_raw(),
 * which the vms-abc SEND SITE TABLE and tests/vmsscs/test_scsd_send_sites.py
 * exist to make impossible. Every one of its call sites now goes through
 * send_frame_vc() -- the CM/join frames are SCS sequenced messages on a formed
 * circuit, which is exactly the traffic p. 2-31 says a circuit carries, so they
 * are CHOKED, not exempt. See the SEND SITE TABLE for the enumerated sites. */

/*
 * ps_learn_sys_addr - record the peer's SCA src-logical address (abs 24) as the
 * 48-bit SCS System Address of the System Block for its node. Replaces the old
 * `memcpy(ps->logical, buf + OFF_HELLO_SRCLOG, 6)`; the bytes stored are
 * identical, the structure they are stored in is the architected one.
 *
 * ONE FAILURE MODE THE OLD memcpy DID NOT HAVE: the store now needs an SB, and
 * scs_pb_learn_system_addr returns NULL if the SB pool is exhausted. If that ever
 * happened, ps_sys_addr() would keep returning zeros and OVMX would put a zero
 * peer-logical on the wire. That must NEVER be silent (CLAUDE.md rule 9 / INV-6),
 * so it is logged as an error here. It is also unreachable at the shipped pool
 * sizes -- SCS_CONFIG_MAX_SB (10) >= OVMX_MAX_PEERS (4) formative SBs + the local
 * node's own SB -- which tests/vmsscs/test_scsd_wire.c asserts at compile time AND
 * by driving a full peer table through this function.
 */
static void ps_learn_sys_addr(struct scs_config *cfg, struct peer_state *ps,
                              const uint8_t *src_logical)
{
    if (ps == NULL || ps->pb == NULL) {
        return;
    }
    if (scs_pb_learn_system_addr(cfg, ps->pb, src_logical) == NULL) {
        log_ts(stderr);
        fprintf(stderr,
                " SCSD-E-NOSB, no system block available for peer"
                " %02x:%02x:%02x:%02x:%02x:%02x -- its SCS system address is NOT"
                " recorded and outgoing frames will carry a zero peer-logical\n",
                ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5]);
        fflush(stderr);
    }
}

/*
 * ps_channel_up - the NISCA channel to this peer is verified. OVMX INFERENCE
 * (labeled): a node exchanging directed HELLOs with us is handling normal SCA
 * communication, which is what "port state = ENABLED" means for a VAX port
 * (p. 2-11..2-12). Nothing here reaches the wire.
 */
static void ps_channel_up(struct peer_state *ps)
{
    ps->channel_up = 1;
    if (ps->pb != NULL) {
        ps->pb->remote_port_state = SCS_PORT_STATE_ENABLED;
    }
}

/* =====================================================================
 * vms-dd5 -- THE CONNECTION DESCRIPTOR LIST AND THE CONNECTION STATE MACHINE
 * =====================================================================
 *
 * WHAT CHANGED AND WHAT DID NOT. vms-e1a built scs_cdt.c (the CDT/CDL/CONID
 * model of pp. 2-28..2-30) but nothing linked it and scsd.c called none of it;
 * scs_cdt.h said so plainly. This block is that wiring, plus the state machine
 * that vms-e1a explicitly left out. It is a RECORDER, not a gate: every
 * `if (!ps->dir_connected)` / `if (!ps->joiner_connected)` guard that decided
 * what OVMX sends is untouched and still decides it, the builders are called
 * with the same arguments in the same order, and each peer's Con.IDs are
 * claimed at their per-peer values (peer slot 0 = the historical fixed values).
 * No byte moves for the single-peer case. See scs_conn.h's WIRE VERDICT.
 *
 * THE MAPPING FROM OBSERVED FRAME TO SCA MESSAGE is the weak claim here and it
 * is stated at each call site below, with its grounding, so it can be checked
 * against the frame that triggers it.
 *
 * vms-694: THE NODE-GLOBAL Con.ID LIMIT IS GONE for the case that mattered.
 * OVMX's Con.IDs used to be node-global macros, so exactly one peer's
 * connections could occupy the CDL slots and a second established member's
 * connection was refused a CDT (scsd_svc_slot_refused) -- the blocker scs_cdt.h
 * recorded and the reason a JOIN to a running 2+-member cluster stalled at
 * status=NEW. Con.IDs are now PER-PEER (PS_*_CONID(ps); see the block at their
 * definition): peer slot i gets its own 0x10-wide low-word block, slot 0 keeps
 * the historical values, so each concurrently-live peer occupies its own CDL
 * slots. The one residual case scsd_svc_slot_refused still covers is a CDL
 * genuinely full, or the same peer opening a SECOND connection of the SAME
 * class (still one per-peer slot per class) -- a distinct, out-of-scope
 * limitation (a real node allocates a fresh pair per connection, vms-298).
 */

/* The node-wide Connection Descriptor List (p. 2-29). File scope so the send
 * helpers can reach it; initialized once in daemon main. */
static struct scs_cdl scsd_cdl;
static int scsd_cdl_ready = 0;

/*
 * ===== vms-7c0: THE PORT DRIVER'S RECEIVE CONTEXT AND ITS DELIVERY LEDGER ====
 *
 * scsd_rx_current is set for the duration of ONE scs_cdl_deliver_message()
 * call and restored afterwards. It exists because p. 2-29 hands the SYSAP input
 * routine "the message" -- the payload -- while OVMX's SYSAP parsers
 * (scs_member_parse) read ABSOLUTE frame offsets and also need the daemon's
 * transmit context to answer on. LABELED an OVMX design choice: VMS passes the
 * CDT address as the context and its port driver has an IRP; scsd has a
 * single-threaded recv loop, so the equivalent is this. The save/restore at the
 * call site makes a nested delivery correct rather than merely improbable.
 *
 * The counters below are the honest record of what the p. 2-29 path actually
 * did on a run. They are file scope for the same reason pb_open_results is:
 * scsd_exit_summary() reports them and tests/vmsscs/test_scsd_wire.c resets and
 * reads them. "The CDL routes messages now" is a claim these numbers make or
 * fail to make -- rx_delivered_message == 0 on a run that joined a cluster
 * would mean the path is decorative.
 */
struct scsd_rx; /* completed below, at the receive-loop context */
struct scsd_rx_frame {
    struct scsd_rx *rx;
    const uint8_t  *frame;
    ssize_t         len;
};
static struct scsd_rx_frame scsd_rx_current;

static unsigned long rx_app_messages = 0;        /* MTYPE 10 frames seen */
static unsigned long rx_delivered_message = 0;   /* ... reaching a SYSAP */
static unsigned long rx_deliver_no_cdt = 0;      /* dest Con.ID names no open connection */
static unsigned long rx_deliver_src_mismatch = 0;/* p. 2-35 source refusal */
static unsigned long rx_deliver_no_routine = 0;  /* CDT carries no input routine */
static unsigned long rx_unknown_mtype = 0;       /* MTYPE outside the observed {0..10} */
/* vms-ec7 (MSCP epic Phase A): the CONTROL half of the p. 4-15 dispatch, made
 * countable at the one place every received frame is classified. MTYPE 10 has
 * had rx_app_messages since vms-7c0; MTYPE 0..9 had nothing, so "which control
 * messages arrive, and how many" was answerable only by reading the run log for
 * the handful that happened to log. rx_control_by_mtype is indexed by MTYPE, so
 * the exit summary prints the received half of the connection dialogue next to
 * the transitions the state machine made on it. */
static unsigned long rx_control_messages = 0;    /* MTYPE 0..9 frames seen */
static unsigned long rx_control_by_mtype[SCS_ENV_MTYPE_CONTROL_MAX + 1] = { 0 };
/* Frames the pre-vms-ec7 connection-control guard would have read a message
 * type out of, and that carry no SCS envelope at all. See the long note at the
 * classifier: this counter is how "the classifier no longer reads a field the
 * frame does not have" stays a MEASUREMENT on this host instead of a claim
 * about the capture corpus. */
static unsigned long rx_control_nonconformant = 0;
static unsigned long sysap_msg_input_calls = 0;  /* the input routine ran */
static unsigned long sysap_cm_messages = 0;      /* ... and it was a CM dialogue frame */

/* Installed on every CDT the five services open (p. 2-29); defined with the
 * receive dispatch, far below, because it drives the whole CM dialogue. */
static void scsd_sysap_msg_input(struct scs_cdt *cdt, const void *msg, size_t msglen,
                                 void *ctx);

/* vms-34b: installed on the MSCP$DISK SERVER CDT (OVMX_MSCP_SERVER_CONID)
 * the moment OVMX accepts a member-opened MSCP$DISK connect. Defined with the
 * receive dispatch, next to scsd_sysap_msg_input, for the same reason. */
static void scsd_mscp_srv_msg_input(struct scs_cdt *cdt, const void *msg,
                                    size_t msglen, void *ctx);

/* vms-600: the MSCP disk-server responder's LIVE block-transfer sender --
 * scs_mscp_srv_xfer_fn's real implementation for a wire connection, as
 * opposed to test_scs_mscp_srv.c's in-memory scs_mscp_srv_blk_sink_xfer().
 * Defined below send_frame_vc() (needs the choke point); forward-declared
 * here so scsd_mscp_srv_state() can install it with scs_mscp_srv_set_xfer(). */
static long scsd_mscp_srv_xfer(void *ctx, const uint8_t buffer_desc[12],
                               uint32_t lbn, const uint8_t *data, size_t len);

/*
 * vms-561: the node's SCS SERVICE PORT -- the object the five architected
 * services (LISTEN, CONNECT, ACCEPT, REJECT, DISCONNECT) run against. One per
 * node, next to the one CDL it drives. See src/vmsscs/include/scs_svc.h.
 *
 * scsd.c is the PORT DRIVER half of p. 2-56's split: the services own the
 * descriptor lifecycle and the connection state, and call back into the
 * scsd_svc_emit_* functions below to build and transmit the SCS control message
 * each transition names. Those emitters are ordinary senders and are named in
 * the SEND SITE TABLE like any other.
 */
static struct scs_svc_port scsd_svc_port;

/*
 * scsd_svc - the port, with its CDL bound on first use.
 *
 * The binding is LAZY rather than done once in main() because main() is not the
 * only thing that starts a node: SCSD_UNIT_TEST renames main() away, and
 * tests/vmsscs/test_scsd_wire.c builds a world by initializing scsd_cdl
 * directly and then calling the production dispatch. A port whose CDL was only
 * ever bound from main() would be descriptorless in every one of those tests --
 * i.e. the entire connection model would silently stop being exercised, which
 * is the failure mode this epic keeps catching. Binding on first use makes the
 * daemon and the tests take the same path.
 *
 * scs_svc_port_init() zeroes the counters, so this must run at most once per
 * CDL; the scsd_svc_port.cdl == NULL guard is what guarantees that.
 */
/* vms-66f: the SCS Process Poller (p. 2-50). Declared up here with the other
 * node-wide state because scsd_sysap_vc_loss(), far above its definition, has
 * to re-enable polling on a lost connection. See the POLLER block further down
 * for what it does and what it deliberately does not do. */
static struct scs_poller scsd_poller;
static int scsd_poller_ready = 0;

/*
 * vms-34b: THE LIVE MSCP DISK-SERVER RESPONDER -- one controller per node,
 * matching the DSRV shape docs/design-mscp-direction.md sec 2 describes (one
 * node, not one per peer: sec 4.8's "load balancing" and per-host HQB/HULB
 * bookkeeping already live INSIDE struct scs_mscp_srv, keyed by the conid
 * scsd_mscp_srv_msg_input() passes in). Lazily initialized for the same
 * reason scsd_svc() is: SCSD_UNIT_TEST and test_scsd_wire.c build a world
 * without ever calling main().
 *
 * vms-600 CLOSES THE GAP THIS COMMENT USED TO DESCRIBE ("no unit is attached
 * here"). A real VAX cannot MOUNT a unit this daemon never claims to serve --
 * so scsd_mscp_srv_state() now attaches a backing store, READ ONLY, from
 * OVMX_MSCP_SRV_UNIT_FILE if that env var is set, and installs the live
 * block-transfer sender below unconditionally (installing it costs nothing
 * when no unit is attached -- READ/WRITE never reach a unit that GET UNIT
 * STATUS never enumerated). Left UNSET (the default, e.g. every existing
 * ctest/CI run), the daemon keeps EXACTLY vms-34b's honest zero-unit posture:
 * no fd is opened, GET UNIT STATUS enumerates nothing, ONLINE/READ/WRITE
 * answer Unit-Offline. Nothing here changes behaviour for a node that never
 * sets the variable.
 *
 * THE VOLUME MUST BE A GENUINE, USABLE ODS-2 VOLUME -- see the
 * [[mscp-serve-actual-volume]] memory and scs_mscp_srv.h design decision (1):
 * MSCP is filesystem-agnostic (it serves logical blocks, not files), and
 * OVMX's own vmsfs is NOT ODS-2-compatible, so the CONTENT that makes a served
 * unit real is that the bytes are a volume real VMS RMS can parse -- produced
 * either by a real VAX (tests/ods2/real_vax_ods2.dsk) or by OVMX's own
 * src/vmsfs/ods2 writer, itself verified against real-VAX MOUNT+DIRECTORY+TYPE
 * (vms-0f3). A blank placeholder file would answer ONLINE/READ honestly at
 * the WIRE level while being INV-6 dishonest at the VOLUME level -- this
 * function does not create or fabricate a volume, only opens whatever
 * OVMX_MSCP_SRV_UNIT_FILE already names, so that dishonesty is the caller's
 * to make, not this daemon's.
 */
static struct scs_mscp_srv scsd_mscp_srv;
static int scsd_mscp_srv_ready = 0;
static int scsd_mscp_srv_unit_fd = -1; /* -1 == nothing attached */

/* vms-600: the live xfer hook's per-command scratch state -- see
 * scsd_mscp_srv_xfer()'s own comment for why one static instance is correct
 * for a single-threaded, synchronous responder. */
static struct scsd_mscp_srv_xfer_ctx {
    int                    sock;
    int                    ifindex;
    struct peer_state     *ps;
    struct scs_mscp_params tmpl;  /* addressing/identity; seq fields overwritten per frame */
    uint32_t               bytes_total;
    uint32_t               bytes_sent;
    uint32_t               src_buf_name;
    uint16_t               conn_const;  /* UNGROUNDED wire field [4:6] -- see scs_mscp_srv.h */
    uint16_t               xfer_const;  /* UNGROUNDED wire field [6:8] -- increments per transfer */
} scsd_mscp_xfer_ctx;
static uint16_t scsd_mscp_xfer_generation = 0;

static void scsd_mscp_srv_maybe_attach(struct scs_mscp_srv *srv)
{
    const char *path = getenv("OVMX_MSCP_SRV_UNIT_FILE");
    if (path == NULL) {
        return; /* the default: no unit, vms-34b's honest zero-unit posture */
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr,
                "SCSD-E-MSCPUNITOPEN, cannot open OVMX_MSCP_SRV_UNIT_FILE"
                " '%s': %s -- serving with zero attached units\n",
                path, strerror(errno));
        return;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)SCS_MSCP_BLOCK_SIZE) {
        fprintf(stderr,
                "SCSD-E-MSCPUNITSIZE, '%s' is not at least one %u-byte block"
                " -- refusing to serve it rather than claim a unit that isn't"
                " there\n",
                path, SCS_MSCP_BLOCK_SIZE);
        close(fd);
        return;
    }
    uint32_t nblocks = (uint32_t)((uint64_t)st.st_size / SCS_MSCP_BLOCK_SIZE);

    int unit = 0;
    const char *unit_env = getenv("OVMX_MSCP_SRV_UNIT");
    if (unit_env != NULL) {
        unit = atoi(unit_env);
    }
    if (unit < 0 || unit > 0xffff) {
        fprintf(stderr,
                "SCSD-E-MSCPUNITNUM, OVMX_MSCP_SRV_UNIT='%s' is out of range\n",
                unit_env);
        close(fd);
        return;
    }

    /* P.UNTI (sec 6.12) must be non-zero and unique to this unit; scs_mscp_srv.h
     * documents OVMX's own controller identity as "OVMX"+n (OVMX_MSCP_SRV_CTLR_ID)
     * -- this follows the same OVMX DESIGN CHOICE pattern rather than inventing
     * an unrelated scheme, since neither is derived from any oracle (OVMX has
     * no real hardware serial number to report). */
    uint64_t unit_id = 0x4F564D5800000000ULL | (uint64_t)(unsigned)(unit + 1);

    if (scs_mscp_srv_attach_fd(srv, (uint16_t)unit, fd, nblocks, unit_id,
                               OVMX_MSCP_SRV_MEDIA_ID, 1u) != 0) {
        fprintf(stderr,
                "SCSD-E-MSCPUNITATTACH, scs_mscp_srv_attach_fd() refused unit"
                " %d from '%s'\n",
                unit, path);
        close(fd);
        return;
    }
    scsd_mscp_srv_unit_fd = fd;
    fprintf(stderr,
            "SCSD-I-MSCPUNIT, serving unit %d from '%s' (%u blocks, %llu"
            " bytes, read-only)\n",
            unit, path, nblocks,
            (unsigned long long)((uint64_t)nblocks * SCS_MSCP_BLOCK_SIZE));
}

static struct scs_mscp_srv *scsd_mscp_srv_state(void)
{
    if (!scsd_mscp_srv_ready) {
        scs_mscp_srv_init(&scsd_mscp_srv, OVMX_MSCP_SRV_CTLR_ID,
                          OVMX_MSCP_SRV_CTLR_TIMEOUT);
        (void)scs_mscp_srv_set_ctlr_profile(&scsd_mscp_srv,
                                            OVMX_MSCP_SRV_CTLR_FLAGS,
                                            OVMX_MSCP_SRV_CTLR_VERSION);
        scsd_mscp_srv_maybe_attach(&scsd_mscp_srv);
        /* Installed unconditionally: a READ can only reach this hook once
         * scs_mscp_srv_handle() has found an online unit, so installing it
         * with nothing attached changes nothing (the same "no unit" answers
         * as before still fire first). */
        (void)scs_mscp_srv_set_xfer(&scsd_mscp_srv, scsd_mscp_srv_xfer,
                                    &scsd_mscp_xfer_ctx);
        scsd_mscp_srv_ready = 1;
    }
    return &scsd_mscp_srv;
}

static struct scs_svc_port *scsd_svc(void)
{
    if (scsd_svc_port.cdl == NULL && scsd_cdl_ready) {
        scs_svc_port_init(&scsd_svc_port, &scsd_cdl);
        /*
         * p. 2-22's "list of listening SYSAPs", which as of vms-7fe IS the
         * p. 2-48 SDIR queue: each of these allocates an SDIR carrying the name
         * plus the CONID of a listening CDT (scs_sdir.h).
         *
         * OVMX LISTENS FOR EXACTLY WHAT IT SERVES, AND NOT MSCP$DISK.
         * vms-7fe was dispatched naming three SYSAPs -- VMS$VAXcluster,
         * MSCP$DISK, SCS$DIRECTORY. Two are registered here and the third is
         * DELIBERATELY NOT, which is a stated deviation, not an oversight:
         *
         *   VMS$VAXcluster  SERVED. scsd.c answers its CONNECT-REQUEST, binds
         *                   the Con.ID pair and drives the add-member burst.
         *   SCS$DIRECTORY   SERVED. scsd.c answers its CONNECT-REQUEST and its
         *                   SCS$DIR_LOOKUP messages. Note that registering it
         *                   is itself the fix for a small lie: before this item
         *                   a lookup for SCS$DIRECTORY got "NOT PRESENT HERE"
         *                   from a hardcoded compare that affirmed only
         *                   VMS$VAXcluster, while the daemon was serving the
         *                   connection at the same moment.
         *   MSCP$DISK       NOT REGISTERED HERE -- and vms-578 leaves that alone
         *                   DELIBERATELY, while recording that the sentence
         *                   vms-7fe wrote to justify it is no longer true of
         *                   this tree.
         *
         *                   vms-7fe wrote: "OVMX has no disk server: there is
         *                   no MSCP responder anywhere in this tree." After the
         *                   worktree-760 merge OVMX DOES answer the
         *                   member-opened MSCP$DISK connect -- op=1
         *                   CONNECT-ECHO then op=4 CONNECT-ACCEPT binding a
         *                   fresh server handle (scs_dir_build_mscp_echo /
         *                   scs_dir_build_mscp_accept). What it still has NO
         *                   implementation of is MSCP disk-server COMMAND
         *                   handling: scs_mscp.c is a CLIENT (it builds SET
         *                   CONTROLLER CHARACTERISTICS / GET UNIT STATUS and
         *                   parses the END responses). So OVMX accepts the
         *                   connection and answers none of its commands.
         *
         *                   THE LOOKUP ANSWER AND THIS LIST THEREFORE DISAGREE,
         *                   and that disagreement is stated rather than hidden.
         *                   The SCS$DIR_LOOKUP handler affirms MSCP$DISK
         *                   (worktree-760, and worktree-760 MEASURED that the
         *                   member does not open the MSCP$DISK connect without
         *                   the HIT -- which is a step on the path to MEMBER).
         *                   This queue does not list it. vms-7fe existed to
         *                   remove exactly that kind of split, so re-creating
         *                   it is a regression on a real invariant.
         *
         *                   IT IS NOT RESOLVED HERE BECAUSE RESOLVING IT IS A
         *                   PRODUCT DECISION: registering MSCP$DISK advertises
         *                   a SYSAP whose commands OVMX cannot serve (an INV-6
         *                   shape, and the standing gate
         *                   test_sdir_lookup_is_answered_from_the_queue asserts
         *                   against it BY NAME); not registering it keeps two
         *                   sources of truth for one wire answer. Registering
         *                   it was tried in this item and reds that gate, which
         *                   is the gate doing its job. Escalated rather than
         *                   decided; OVMX_DISKLESS=1 already withdraws the
         *                   lookup-side affirmative for bisecting.
         *
         *                   SUPERSEDED (vms-34b, operator ruling 2026-08-06):
         *                   the product decision above came back BUILD THE
         *                   FULL SERVER, and "OVMX cannot serve" stopped being
         *                   true once scsd_mscp_srv_msg_input() (below) gave
         *                   the accept path below a real message-input
         *                   routine. The INV-6 gap this paragraph describes is
         *                   closed, not by picking the cheaper branch, but by
         *                   making the responder real -- see
         *                   ovmx_mscp_server_enabled()'s comment for the
         *                   current state and test_sdir_lookup_is_answered_
         *                   from_the_queue / test_sdir_refuses_a_connect_
         *                   request_for_an_unlisted_sysap (rewritten,
         *                   tests/vmsscs/test_scsd_wire.c) for the gates that
         *                   now assert the new truth. Left in place as the
         *                   record of what was tried and why it wasn't done
         *                   sooner -- not a currently-accurate description.
         */
        (void)scs_listen(&scsd_svc_port, "VMS$VAXcluster", NULL, NULL);
        (void)scs_listen(&scsd_svc_port, "SCS$DIRECTORY", NULL, NULL);
        /* vms-578/vms-34b: registered ONLY when OVMX is configured as an
         * MSCP$DISK server, so the queue always describes what the daemon
         * will actually do. See ovmx_mscp_server_enabled() (default ON as of
         * vms-34b). */
        if (ovmx_mscp_server_enabled()) {
            (void)scs_listen(&scsd_svc_port, "MSCP$DISK", NULL, NULL);
        }
    }
    return &scsd_svc_port;
}

/*
 * vms-7fe: the p. 2-48 scan's outcomes, counted for the exit report. These are
 * the numbers that say what the SDIR queue actually did on a run -- in
 * particular sdir_busy_replies, which the OVMX design-choice-3 note in
 * scs_sdir.h predicts is 0 because the daemon's receive loop cannot reach it.
 * tests/vmsscs/test_scsd_wire.c turns that prediction into a check: it sums
 * this counter across every case in the file and asserts the total is 0.
 */
static unsigned long sdir_connect_scans = 0;   /* inbound CONNECT_REQs scanned */
static unsigned long sdir_no_such_sysap = 0;   /* p. 2-48 refusals emitted */
static unsigned long sdir_busy_replies = 0;    /* p. 2-50 refusals emitted */
static unsigned long sdir_refusals_unsent = 0; /* refusal the circuit would not carry */

/* Counters for the end-of-run summary. */
static unsigned long conn_transitions = 0;
static unsigned long conn_illegal_events = 0;
static unsigned long conn_unemitted_actions = 0;

/* vms-6b3: p. 2-26's 16-bit reason code, as RECEIVED. `seen` counts every
 * REJECT_REQ/DISCONNECT_REQ addressed to one of our Con.IDs whose field we
 * decoded; `nonzero` counts those where the peer actually supplied a reason.
 * Both are printed in the exit summary because "0 nonzero" is the answer our
 * whole lab capture set gives (scs_reason.h) and it must be VISIBLE rather than
 * inferred from the absence of a log line. */
static unsigned long conn_reason_seen = 0;
static unsigned long conn_reason_nonzero = 0;

/* vms-591: the DISCONNECT dialogue (Figure 2-16), counted for the exit summary.
 * These are the numbers that say whether the teardown was symmetric or whether
 * OVMX only ever answered: `req_recv` counts peer DISCONNECT_REQs delivered to
 * the machine, `rsp_sent` OVMX's answers, `req_sent` OVMX's OWN disconnect
 * calls (which is the half that did not exist before this item), `rsp_recv` the
 * peer's answers to those, `closed` connections that actually reached CLOSED,
 * and `simul` the p. 2-27 simultaneous case. `shutdown_pending` is how many
 * connections were still not CLOSED when the shutdown wait timed out. */
static unsigned long disc_req_recv = 0;
static unsigned long disc_rsp_sent = 0;
static unsigned long disc_req_sent = 0;
static unsigned long disc_rsp_recv = 0;
static unsigned long disc_closed = 0;
static unsigned long disc_simultaneous = 0;
static unsigned long disc_shutdown_pending = 0;

/* vms-abc: p. 2-31 message-guarantee enforcement, counted for the exit report. */
static unsigned long vc_seq_gaps = 0;    /* sequentiality failures detected */
static unsigned long vc_breaks = 0;      /* circuits explicitly broken */
static unsigned long vc_conns_broken = 0;/* connections broken with them */
static unsigned long sysap_vc_loss_notifications = 0; /* SYSAP handlers actually invoked */
static unsigned long vc_sends_refused = 0; /* frames send_frame_vc() would not transmit */

/*
 * ===== vms-aa1: THE FLOW-CONTROL ACCOUNT, DRIVEN BY TRAFFIC THAT REALLY FLOWS
 *
 * vms-76e built the pp. 2-43..2-45 debit/credit account in the CDT and vms-1d2
 * the Credit Wait, both unit tested, and the vms-096 ledger then measured the
 * thing that made them decorative: NO PRODUCTION CALLER. Nothing debited a Send
 * Credit on a real send, nothing piggybacked a Pending Receive Credit onto a
 * real outbound frame, nothing added an inbound credit field to any account.
 * The counters below are what makes the difference measurable rather than
 * asserted -- they are printed unconditionally in the exit summary, so a run
 * whose account never moves says so in its own log (INV-6).
 *
 * WHERE THE TWO HALVES ARE. The send half is scsd_credit_stamp_outbound(),
 * called from send_frame_vc() -- the p. 2-31 choke point every connection-
 * oriented sender already goes through, so a sender added tomorrow is accounted
 * by construction. The receive half is scsd_credit_bank_inbound(), called from
 * scsd_handle_frame() at the point scs_rx_parse() has decoded the envelope.
 *
 * WHICH FRAMES, AND WHY NOT ALL OF THEM. The credit field is grounded at SCA
 * [48:50] (scs_credit.h WIRE VERDICT / tools/scs_credit_measure.py -- NOT
 * re-derived here), but it does not carry the same QUANTITY on every message
 * type, and spec sec 4(h)(1c) measures the difference over real-VAX frames:
 *
 *   MTYPE 10 (p. 4-13 APPLICATION MESSAGE) -- the SYSAP data class, and the
 *     only class the p. 2-44 piggyback is about. This is the class OVMX both
 *     sends (the 190-byte CM frames, scs_member.c) and receives. STAMPED on
 *     send, BANKED on receive.
 *   MTYPE 2 (ACCEPT_REQ) -- the credit field is the number of Send Credits
 *     being EXTENDED at connection formation (p. 2-43; spec sec 4(g)'s tunable
 *     match, 10 = CLUSTER_CREDITS, 8 = MSCP_CREDITS). A different quantity.
 *     BANKED on receive through scs_credit_grant_from_peer(); NEVER stamped,
 *     because overwriting an extension with a pending count would emit a lie.
 *   MTYPE 0 (CONNECT_REQ) -- same quantity as 2, but its DESTINATION Con.ID is
 *     0 by construction (the target CDT does not exist yet), so it resolves to
 *     no CDT and is banked nowhere. Stated so the absence is not read as an
 *     oversight.
 *   MTYPE 5 and 7 -- credit == 0 in 4523/4523 and 734/734 real-VAX frames
 *     (sec 4(h)(1c)). Stamping a live count there would put a shape on the wire
 *     that has never been observed. LEFT ALONE.
 *   MTYPE 1,3,4,6,8,9 -- 8/9 carry a constant 1 whose meaning is UNNAMED (the
 *     "special credit message" candidate is WEAKENED, not confirmed -- sec
 *     4(h)(1c)); the rest carry 0. Reading an unnamed constant as a credit
 *     grant would be a wire claim with no observation behind it. LEFT ALONE.
 *
 * KILL SWITCH: OVMX_NO_CREDIT_ACCOUNTING=1 (scs_credit.h). Both halves return
 * before touching anything, so the daemon looks up no CDT, debits nothing,
 * stamps nothing, and every counter below stays 0 -- the bytes OVMX transmits
 * are then EXACTLY the bytes its builders produced, which is what makes the
 * switch a usable matched control on the lab (guardrail 23).
 */
static unsigned long credit_send_stamped = 0;   /* outbound MTYPE-10 frames that carried a live piggyback */
static unsigned long credit_send_units = 0;     /* total Pending Receive Credits piggybacked */
static unsigned long credit_send_starved = 0;   /* outbound MTYPE-10 frames with no Send Credit (p. 2-45) */
static unsigned long credit_send_no_cdt = 0;    /* outbound MTYPE-10 frames whose SOURCE Con.ID resolved to no CDT */
static unsigned long credit_recv_banked = 0;    /* inbound MTYPE-10 frames whose credit field was banked */
static unsigned long credit_recv_units = 0;     /* total Send Credits banked from those fields */
static unsigned long credit_grants_recv = 0;    /* inbound ACCEPT_REQs that extended Send Credits */
static unsigned long credit_grant_units = 0;    /* total Send Credits those extensions carried */
static unsigned long credit_buffers_released = 0; /* p. 2-43 SYSAP buffer releases */

/*
 * vms-abc: THE SYSAP's VC-LOSS ERROR HANDLER, and the first one OVMX has ever
 * installed.
 *
 * p. 2-28: "the VMS implementation of SCA stores the address of the interested
 * SYSAP's error handler for virtual circuit loss in the CDT itself. This address
 * is supplied by a SYSAP as an argument to the VMS implementations of both the
 * CONNECT and ACCEPT services." p. 2-31: when a message guarantee fails, "every
 * connection supported by this virtual circuit is also broken, and the SYSAPs
 * participating in these connections are notified of the event."
 *
 * BEFORE THIS ITEM no CDT carried a VC-loss handler at all, so the notification
 * half of p. 2-31 was implemented and DEAD: scs_cdl_vc_loss() walked the Path
 * Block's connection queue, counted what it found and notified nobody, because
 * every vc_loss_handler it tested was NULL. conn_bind() below now installs THIS
 * function on every CDT it creates -- the first SYSAP error handler OVMX has
 * ever had.
 *
 * WHO CALLS THE SCAN. An earlier draft of this comment said scs_cdl_vc_loss()
 * "had no production caller". That was true when it was written and is FALSE on
 * this branch: vms-17f landed underneath this item and brought one. There are
 * TWO production callers, and this handler is now reached from BOTH:
 *
 *   1. scs_vc_break()  (src/vmsscs/scs_vc.c) -- THIS item's caller. A p. 2-31
 *      message-guarantee failure: a receive-side sequence gap detected in
 *      scsd_handle_frame(), or retransmit exhaustion in scsd_retransmit_tick().
 *      Gated by OVMX_NO_VC_BREAK.
 *   2. scs_pb_depart() (src/vmsscs/scs_depart.c) -- vms-17f's caller, the
 *      p. 2-28 ordered teardown, reached from scsd_peer_departure_sweep() below
 *      when a peer goes silent past the listen timeout.
 *      Gated by OVMX_NO_PEER_DEPART.
 *
 * THE CONSEQUENCE ON CALLER (2), which this item introduced and therefore owns:
 * because installation happens in conn_bind() rather than in the break path,
 * EVERY peer departure carrying bound connections now invokes this handler,
 * moves sysap_vc_loss_notifications and emits SCSD-W-SYSAPVCLOSS. vms-17f's
 * departures used to be silent here. That is asserted through the production
 * sweep by test_departure_notifies_the_sysaps() in tests/vmsscs/test_scsd_wire.c.
 *
 * WHY OVMX_NO_VC_BREAK DOES NOT GATE INSTALLATION -- decided, not defaulted.
 * The obvious alternative is to skip the install when this item's kill switch is
 * set, so the switch covers everything the item changes. Rejected, for three
 * reasons:
 *   - The switch's contract is "do not BREAK a circuit on a message-guarantee
 *     failure"; it announces itself as SCSD-W-VCBREAKOFF. A departure is not a
 *     message-guarantee failure.
 *   - Notifying the SYSAP on a departure is p. 2-28 behaviour that vms-17f
 *     explicitly wanted and could not have, only because no handler existed.
 *     Gating installation here would make THIS item's kill switch silently
 *     disable a SIBLING item's intended behaviour -- a switch that does more
 *     than revert its own item is worse than no switch.
 *   - It would change WHAT IS BOUND rather than what happens on failure: a CDT
 *     with a NULL vc_loss_handler is a structurally different object, so the
 *     switch would perturb steady state, not just the failure path.
 * The property that actually matters is that no path reaching this handler is
 * UNGATED, and that holds: caller (1) is gated by OVMX_NO_VC_BREAK, caller (2)
 * by OVMX_NO_PEER_DEPART. Both gates are RUN with the counter confirmed to stay
 * at 0, including the deliberately-not-gated combination (departure sweep with
 * OVMX_NO_VC_BREAK=1 set, where the notification MUST still fire).
 *
 * WHAT IT DOES: clears the bound-connection flag for exactly the connection
 * that was broken, so the daemon's view of what is bound matches the CDT it
 * just closed.
 *
 * WHAT IT DOES **NOT** DO, and an earlier revision of this comment claimed it
 * did -- the claim was FALSE and was measured false. It said these three
 * booleans "are what actually decide whether scsd.c sends anything on a
 * connection, so leaving them set ... would keep OVMX transmitting". Two of the
 * three are read NEGATED: the directory CONNECT-REQUEST branch gates on
 * `!ps->dir_connected`, and the prompt-joiner branch and
 * scsd_joiner_retransmit_pending() gate on `!ps->joiner_connected`. Clearing
 * them RE-ARMS those sends. MEASURED: with no other guard, replaying the peer's
 * captured SCS$DIRECTORY CONNECT-REQUEST after a seq-gap break made OVMX emit a
 * SECOND directory CONNECT-RESPONSE (dir_conn_resp_sent 1 -> 2) on a Path Block
 * whose vc_state was CLOSED.
 *
 * AND `ps->connected` DOES NOT SUPPRESS ANYTHING EITHER, which a previous
 * revision of this comment also got wrong: it claimed `ps->connected` (read
 * POSITIVE at the 0x4b branch) was "suppressed by the clear". It is not. The
 * branch that ANSWERS the member's 0x4b CONNECT-REQUEST is gated on
 * `v.remote_conid == 0` and never reads ps->connected at all -- it SETS it. So
 * replaying the peer's captured VMS$VAXcluster connect after a break re-armed
 * ps->connected 0 -> 1, which in turn re-opened the 0x81 CM-response path that
 * IS gated on it. MEASURED, same broken circuit: connect_resp_sent 1 -> 2,
 * credit_sent 2 -> 3, two frames on the wire, member CDT back to ACCEPT SENT.
 *
 * WHAT ACTUALLY ENFORCES p. 2-31 ("Any attempt to send a message from one port
 * to another in the absence of a virtual circuit will fail") is the CLOSED Path
 * Block, consulted at ONE choke point that every SCS-layer send now goes
 * through: send_frame_vc(). See its SEND SITE TABLE for the full census of
 * senders and the two justified exemptions (VC formation and NISCA HELLO).
 * These three booleans decide only which SYSAP dialogue the daemon believes is
 * bound; they gate no send that the choke point does not already refuse.
 *
 * WHAT IT DELIBERATELY DOES NOT DO, so nobody reads more into it: it does not
 * free the peer slot, does not release the CDT, and does not re-drive a join.
 * Reopening after a loss is vms-17f's teardown/rejoin path, not this handler's.
 */
static void scsd_sysap_vc_loss(struct scs_cdt *cdt, void *ctx)
{
    struct peer_state *ps = (struct peer_state *)ctx;
    const char *which = "?";

    sysap_vc_loss_notifications++;
    if (ps != NULL) {
        if (cdt == ps->cdt_dir) {
            ps->dir_connected = 0;
            which = "SCS$DIRECTORY";
        } else if (cdt == ps->cdt_member) {
            ps->connected = 0;
            which = "VMS$VAXcluster (member-opened)";
        } else if (cdt == ps->cdt_joiner) {
            ps->joiner_connected = 0;
            which = "VMS$VAXcluster (joiner-opened)";
            /* p. 2-50: "If that connection is lost, SYSAP_A has the option of
             * once again requesting its Process Poller to look for SYSAP_X on
             * NODE_X." OVMX exercises that option. */
            if (scsd_poller_ready) {
                scs_poll_connection_lost(&scsd_poller, "VMS$VAXcluster",
                                         ps_sys_addr(ps));
            }
        }
    }
    log_ts(stdout);
    printf(" SCSD-W-SYSAPVCLOSS, SYSAP notified: connection %s (conid=0x%08X)"
           " is broken; it is no longer bound\n",
           which, cdt != NULL ? (unsigned)cdt->local_conid : 0u);
    fflush(stdout);
}

/*
 * vms-17f: the p. 2-20/2-21 open transitions this run actually took, indexed by
 * enum scs_open_result, plus the departure sweep's tallies. File scope for the
 * same reason as the three above: scsd_vc_on_open() is handed a const context
 * and the sweep runs from main()'s timer block, so there is no one `rx` both can
 * reach. tests/vmsscs/test_scsd_wire.c resets and reads them.
 *
 * These exist because "the REFRESH branch is reachable now" is precisely the
 * kind of claim this epic has repeatedly sent back for being unmeasured. The
 * number is the claim.
 */
static unsigned long pb_open_results[3] = {0, 0, 0}; /* NEW_SB, EXISTING_SB, REFRESHED */
static unsigned long pb_open_errors = 0;
/* vms-22e: opens ABANDONED by the p. 2-21 footnote masquerade tests. Separate
 * from pb_open_errors because an abandonment is a RULE firing, not a fault, and
 * because "the wire has never produced one" is a claim this counter measures
 * rather than asserts -- see the reachability block on scsd_vc_on_open(). */
static unsigned long pb_open_masquerades = 0;
static unsigned long peer_departures = 0;
static unsigned long depart_connections_lost = 0;
static unsigned long depart_refusals = 0;

/*
 * ===== vms-561: SCSD AS THE PORT-DRIVER HALF OF THE FIVE SERVICES =====
 *
 * p. 2-56 splits the work and this file is on the far side of the split:
 *
 *   "The SCS CONNECT and ACCEPT services each result in the allocation of a CDT
 *    for new connections. The SCS DISCONNECT service results in releasing CDTs.
 *    Thus, the SYSAP calling interface for each of these services is in
 *    SYS$SCS. However, the actual dialogue for connection formation and
 *    disconnect is managed by SCS code in the port driver."
 *
 * So conn_bind() is GONE. Nothing in this file allocates, binds or releases a
 * CDT any more, and nothing here decides a connection's state: scs_connect()
 * and scs_accept() (src/vmsscs/scs_svc.c) do both, and call back into the three
 * scsd_svc_emit_* functions below for the frames. What is left here is frame
 * assembly, transmission and the peer bookkeeping that is genuinely the
 * daemon's -- ps->dir_connected, ps->joiner_req_seq, the run counters.
 *
 * The p. 2-28 VC-loss error handler ("supplied by a SYSAP as an argument to ...
 * both the CONNECT and ACCEPT services") is now passed the way the book says,
 * as args.vc_loss. As of vms-7c0 so is the p. 2-29 MESSAGE input routine:
 * args.msg_input carries scsd_sysap_msg_input() at all four sites, received
 * application messages are dispatched through the CDL rather than by Con.ID
 * comparison, and scs_cdl_deliver_message() has a production caller. args.
 * dgram_input is still NULL -- OVMX cannot identify an application datagram on
 * this wire, and the census that says so is in scs_rx.h.
 *
 * scsd_svc_mark / scsd_svc_settle - the exit summary's three counters
 * (conn_transitions / conn_illegal_events / conn_unemitted_actions) used to be
 * incremented by conn_step(). The services keep the same tallies on the port,
 * so these two take the delta across one service call and fold it into the
 * daemon's counters. Sampling the delta rather than reading the port directly
 * keeps the counters resettable by a test that resets only scsd.c's.
 */
struct scsd_svc_mark {
    unsigned long transitions;
    unsigned long illegal;
};

static struct scsd_svc_mark scsd_svc_mark(void)
{
    struct scsd_svc_mark m;
    m.transitions = scsd_svc_port.transitions;
    m.illegal = scsd_svc_port.illegal;
    return m;
}

static void scsd_svc_settle(struct scsd_svc_mark m)
{
    conn_transitions += scsd_svc_port.transitions - m.transitions;
    conn_illegal_events += scsd_svc_port.illegal - m.illegal;
}

/*
 * scsd_svc_no_builder - the emitters' answer for an SCS control message OVMX
 * cannot build. Returns SCS_SVC_EMIT_NOBUILDER, so the connection still takes
 * the transition (it really is in the new state) while the run log records that
 * no frame went out. This is conn_step()'s old SCSD-W-CONNNOACT line, moved to
 * the one place that actually knows whether a builder exists.
 */
static int scsd_svc_no_builder(struct scs_cdt *cdt, enum scs_conn_action act)
{
    conn_unemitted_actions++;
    log_ts(stdout);
    printf(" SCSD-W-CONNNOACT, conid=0x%08X: the state machine requires"
           " '%s' here and OVMX has no builder for it -- nothing was sent\n",
           cdt != NULL ? (unsigned)cdt->local_conid : 0u,
           scs_conn_action_name(act));
    fflush(stdout);
    return SCS_SVC_EMIT_NOBUILDER;
}

/*
 * scsd_svc_slot_refused - the SCS_SVC_NOCDT log, which conn_bind() used to
 * carry. vms-694 made Con.IDs PER-PEER, so a DIFFERENT peer's connection no
 * longer collides (that was the join-blocking bug). This now fires only when
 * the CDL is genuinely full, or the SAME peer opens a SECOND connection of the
 * SAME class -- one per-peer slot per class -- which is the out-of-scope
 * per-connection-allocation limitation (vms-298). The frame still went out (the
 * state machine is a recorder, never a gate); what is lost is the tracking.
 */
static void scsd_svc_slot_refused(uint32_t local_conid, const char *local_sysap)
{
    log_ts(stderr);
    fprintf(stderr,
            " SCSD-W-CONNSLOT, CDL slot for Con.ID 0x%08X (%s) is already"
            " claimed -- the same peer/class Con.ID is still live, so this"
            " connection is NOT tracked by the connection state machine\n",
            (unsigned)local_conid, local_sysap);
    fflush(stderr);
}

/*
 * The wire->event mapping lives in scs_conn.c (scs_conn_event_for_msgtype), NOT
 * here: it is a pure function of one wire field, it carries the grounding for
 * every value in docs/cluster-protocol-spec.md sec 4(h)(1a), and keeping it in
 * the library is what lets tests/vmsscs/test_scs_dir.c and test_scs_connect.c
 * assert that the frames OVMX BUILDS classify as the SCA messages the state
 * machine thinks they are -- without a socket.
 *
 * conn_step - drive the machine and account for the result. `emitted` names the
 * frame the daemon actually put on the wire for this step, or NULL if it sent
 * nothing; when the machine's action names a frame OVMX has no builder for,
 * that is LOGGED as an unemitted action instead of being passed over in
 * silence. That log line is the honest record of the OVMX/SCA divergence, and
 * on the 0x4b connections it fires on every run.
 */
static struct scs_conn_transition conn_step(struct scs_cdt *cdt,
                                            enum scs_conn_event ev,
                                            const char *emitted)
{
    struct scs_conn_transition t;
    memset(&t, 0, sizeof(t));
    if (cdt == NULL) {
        t.illegal = 1;
        return t;
    }
    t = scs_conn_fsm_step(cdt, ev);
    if (t.suppressed) {
        return t;
    }
    conn_transitions++;
    if (t.illegal) {
        conn_illegal_events++;
        return t;
    }
    if (t.action != SCS_CONN_ACT_NONE && emitted == NULL) {
        conn_unemitted_actions++;
        log_ts(stdout);
        printf(" SCSD-W-CONNNOACT, conid=0x%08X: the state machine requires"
               " '%s' here and OVMX has no builder for it -- nothing was sent\n",
               (unsigned)cdt->local_conid, scs_conn_action_name(t.action));
        fflush(stdout);
    }
    return t;
}

/*
 * vms-694 (§4(O.7)) -- REFLECT THE MODELLED VMS$VAXcluster CONNECTION IN OVMX'S
 * OWN MEMBERSHIP STATE.
 *
 * The peer holds the VMS$VAXcluster SYSAP connection OPEN (its SDA shows
 * `VMS$VAXcluster -> OVMX 0002 open` for the whole run, §4(O.6)). On OVMX's side
 * that connection is the CDT it opened, cdt_joiner, and the daemon's own
 * connection state machine DOES drive it CLOSED -> CONNECT SENT -> CONNECT ACK
 * -> OPEN off the member's real CONNECT_RSP + ACCEPT_REQ (proven by the run's
 * CONNFSM history, and by test_captured_ovmx_accept_req_opens_the_joiner). What
 * was MISSING was any DURABLE record on the peer_state that this happened: the
 * only aggregate signals, ps->connected and a raw exit-time read of the CDT,
 * both describe a DIFFERENT or a POST-TEARDOWN state --
 *   - ps->connected tracks the *member-opened* VMS$VAXcluster connection
 *     (cdt_member), which the add-member wire never creates (the member
 *     reciprocates on OUR joiner VC), so it stays 0; and
 *   - the CDT sampled at exit reads CLOSED because the daemon gracefully
 *     DISCONNECTs at shutdown (§4(O.7)).
 * Together they made every reader (this item's own dispatch included) conclude
 * "OVMX's state machine never models the connection", which is false.
 *
 * This helper latches the honest fact the moment the modelled connection reaches
 * OPEN, so OVMX's self-view matches the connection the peer holds open and the
 * fact survives the teardown sample. It reads the CDT the state machine actually
 * drove; it does not fabricate a connection (anti-LARP: cdt_joiner genuinely
 * reaches OPEN on the wire). Idempotent; logs once per admission.
 */
static void ps_note_vaxcluster_open(struct peer_state *ps)
{
    if (ps == NULL || ps->vaxcluster_open_reached) {
        return;
    }
    enum scs_conn_state js = (ps->cdt_joiner != NULL)
                                 ? scs_conn_state_of(ps->cdt_joiner)
                                 : SCS_CONN_CLOSED;
    enum scs_conn_state ms = (ps->cdt_member != NULL)
                                 ? scs_conn_state_of(ps->cdt_member)
                                 : SCS_CONN_CLOSED;
    if (js != SCS_CONN_OPEN && ms != SCS_CONN_OPEN) {
        return;
    }
    ps->vaxcluster_open_reached = 1;
    /* vms-c7d: the CM's SCS connection to this remote CM is OPEN -> the CSB
     * reaches OPEN and this node is a held cluster member (transcript p. 7-24).
     * A later reconnect returns here through the same latch. */
    scs_csb_connectivity_gained(&ps->csb, monotonic_ms());
    log_ts(stdout);
    printf(" SCSD-I-VAXCLMEMBER, VMS$VAXcluster SYSAP connection reached OPEN"
           " (conid=0x%08X %s) -- OVMX now models itself as a connected cluster"
           " member of this peer; this is the mirror of the peer's own"
           " 'VMS$VAXcluster -> OVMX 0002 open' CDT (spec 4(O.7))\n",
           (unsigned)(js == SCS_CONN_OPEN && ps->cdt_joiner != NULL
                          ? ps->cdt_joiner->local_conid
                          : (ps->cdt_member != NULL ? ps->cdt_member->local_conid : 0u)),
           js == SCS_CONN_OPEN ? "joiner-opened" : "member-opened");
    fflush(stdout);
}

/*
 * scsd_recnx_note_vc_loss - vms-c7d: a virtual circuit to a remote Connection
 * Manager has broken for a reason that is NOT a "last gasp" departure (a
 * sequentiality or delivery guarantee failure, p. 2-31). Drive this peer's CSB
 * to WAIT so the RECNXINTERVAL reconnect loop runs and MEMBERSHIP IS HELD across
 * the reconnect period (transcript p. 7-30: "Do not presume that the remote
 * system has left ... the local Connection Manager will attempt once a second to
 * establish another connection"). The once-a-second beat and the period expiry
 * are scsd_recnx_tick()'s job; this only enters the wait.
 *
 * OVMX_NO_RECNX_RECONNECT=1 (scs_csb_connectivity_lost's kill switch, guardrail
 * 23) reverts this to the pre-vms-c7d behaviour: membership is dropped at once
 * and a transition is proposed, with no WAIT and no reconnect.
 */
static void scsd_recnx_note_vc_loss(struct peer_state *ps, uint64_t now_ms)
{
    if (ps == NULL) {
        return;
    }
    enum scs_recnx_action a =
        scs_csb_connectivity_lost(&ps->csb, now_ms, /*last_gasp=*/0);
    log_ts(stdout);
    if (ps->csb.state == SCS_CSB_WAIT) {
        printf(" SCSD-I-RECNXWAIT, connectivity to %02x:%02x:%02x:%02x:%02x:%02x"
               " lost -- CSB -> WAIT, membership HELD; reconnecting once/sec for"
               " up to %u s (max local RECNXINTERVAL %u, remote %u) (p. 7-30)\n",
               ps->pb->remote_port_addr[0], ps->pb->remote_port_addr[1],
               ps->pb->remote_port_addr[2], ps->pb->remote_port_addr[3],
               ps->pb->remote_port_addr[4], ps->pb->remote_port_addr[5],
               scs_recnx_timeout_secs(ps->csb.local_recnxinterval,
                                      ps->csb.remote_port_value),
               ps->csb.local_recnxinterval, ps->csb.remote_port_value);
    } else {
        printf(" SCSD-I-RECNXDROP, connectivity to %02x:%02x:%02x:%02x:%02x:%02x"
               " lost -- %s [" SCS_RECNX_NO_RECONNECT_ENV "=1: membership dropped"
               " immediately, no reconnect]\n",
               ps->pb->remote_port_addr[0], ps->pb->remote_port_addr[1],
               ps->pb->remote_port_addr[2], ps->pb->remote_port_addr[3],
               ps->pb->remote_port_addr[4], ps->pb->remote_port_addr[5],
               scs_recnx_action_name(a));
    }
    fflush(stdout);
}

#ifdef SCSD_UNIT_TEST
/*
 * vms-7be TRANSMIT CAPTURE SEAM -- compiled ONLY into tests/vmsscs/test_scsd_wire.c,
 * which is the single translation unit that defines SCSD_UNIT_TEST. The daemon
 * never defines it, so the production build of send_frame_raw below is byte-for-byte
 * the code that shipped before this item.
 *
 * WHY: the frames OVMX puts on the wire are assembled INSIDE this file (dst MAC
 * from ps_port_addr(), SCA peer-logical from ps_sys_addr()) and then handed to an
 * AF_PACKET socket that needs CAP_NET_RAW, which CI does not have. Substituting the
 * sendto() with a capture buffer lets a test drive the REAL scsd.c senders and
 * inspect the REAL bytes, instead of re-deriving them in the test.
 */
uint8_t  scsd_test_last_frame[SCA_FRAME_MAX];
uint8_t  scsd_test_last_dst[6];
size_t   scsd_test_last_len;
unsigned scsd_test_frames;
/*
 * vms-aa1: THE LAST FEW FRAMES, not just the last one. One production call can
 * emit several frames (cm_send_config_burst emits three), and a per-frame wire
 * field -- the p. 2-44 credit piggyback is the first one OVMX has -- differs
 * BETWEEN them by design. A single-frame capture can only show the last value,
 * so a test asserting that the field VARIES across a burst would have to
 * re-derive the other frames instead of reading them. This is the same seam,
 * widened; it exists only in the SCSD_UNIT_TEST translation unit.
 */
#define SCSD_TEST_RING 8
uint8_t  scsd_test_ring[SCSD_TEST_RING][SCA_FRAME_MAX];
size_t   scsd_test_ring_len[SCSD_TEST_RING];
/* vms-94c: DLM cross-node dispatch capture. Under SCSD_UNIT_TEST,
 * scsd_dlm_dispatch_to_executive() records the decoded request here instead of
 * issuing the real /dev/vms ioctl (CI has no /dev/vms), so a test can assert
 * node B decoded the received DLM message to the right fields and dispatched it
 * to the executive. */
struct scs_dlm_msg scsd_test_last_dlm;
unsigned scsd_test_dlm_dispatches;
#endif

/*
 * send_frame_raw - THE TRANSPORT, and nothing else: hand a fully-built Ethernet
 * frame to a specific unicast MAC on ifindex. It applies NO policy.
 *
 * vms-abc RENAMED IT from send_frame_to, deliberately and mechanically. Every
 * SCS-layer send in this file now goes through send_frame_vc() below, which
 * asks the peer's Path Block whether a virtual circuit exists before any byte
 * leaves (p. 2-31). The rename is what makes the remaining direct callers
 * COUNTABLE: tests/vmsscs/test_scsd_send_sites.py attributes every
 * send_frame_raw() call to its enclosing function and reds unless that set is
 * exactly the EXEMPT list in the SEND SITE TABLE below. Under the old name a
 * new send site was indistinguishable from an old one.
 *
 * THE DIRECT CALLERS, and how many calls each makes: send_frame_vc() 1,
 * send_frame_channel() 1, scsd_vc_emit() 2. Those three counts are PINNED in
 * that script (EXEMPT_CALLS), so an extra raw send added inside an already
 * exempt function -- the one way to get an unguarded frame past a structural
 * check that only looks at function names -- reds too. The number of sends
 * routed through the choke point is reported by the script but deliberately
 * NOT pinned: adding one is safe by construction.
 */
static ssize_t send_frame_raw(int sock, int ifindex, const uint8_t mac[6],
                              const uint8_t *frame, size_t len)
{
#ifdef SCSD_UNIT_TEST
    (void)sock;
    (void)ifindex;
    if (len > sizeof(scsd_test_last_frame)) {
        return -1;
    }
    memcpy(scsd_test_last_dst, mac, 6);
    memcpy(scsd_test_last_frame, frame, len);
    scsd_test_last_len = len;
    memcpy(scsd_test_ring[scsd_test_frames % SCSD_TEST_RING], frame, len);
    scsd_test_ring_len[scsd_test_frames % SCSD_TEST_RING] = len;
    scsd_test_frames++;
    return (ssize_t)len;
#else
    /* vms-838: the AF_PACKET sockaddr_ll build + sendto() that used to live
     * here moved to scs_datalink_send()'s Linux backend (byte-for-byte the
     * same construction) -- see scs_datalink.c. This keeps send_frame_raw()
     * as the choke point tests/vmsscs/test_scsd_send_sites.py's SEND SITE
     * TABLE names, while letting the actual platform primitive (sendto() on
     * Linux, write() on NetBSD's bpf) live behind one header shared with a
     * future DECnet Phase IV (vms-30e) sender. That script's own vms-838
     * update explains how it now covers the moved primitive. */
    return scs_datalink_send(sock, ifindex, SCA_ETHERTYPE, mac, frame, len);
#endif
}

/*
 * ===== vms-abc: THE ONE PLACE p. 2-31 IS ENFORCED ON THE TRANSMIT PATH =====
 *
 * p. 2-31: "Any attempt to send a message from one port to another in the
 * absence of a virtual circuit will fail." NOT "any attempt to send a directory
 * reply". The rule is about the circuit, so the enforcement has to be about the
 * circuit too -- one predicate, consulted by every SCS-layer sender, rather
 * than a guard bolted onto whichever branch was last caught misbehaving.
 *
 * WHY THIS SHAPE, stated as the history that produced it. This item first
 * guarded ONE path (the SCS$DIRECTORY CONNECT-REQUEST reply). Re-measuring the
 * SAME broken circuit then found the identical defect on three more: the 0x48
 * credit-return, the 0x4b VMS$VAXcluster CONNECT-RESPONSE (which additionally
 * re-armed ps->connected and re-opened the CM 0x81 path), and the
 * SCS$DIR_LOOKUP reply. The defect was never per-path: NO SEND SITE CONSULTED
 * THE PATH BLOCK. Guarding sites one at a time cannot terminate, because the
 * next site added is unguarded by default. Routing them through one function
 * inverts that: a new site is guarded by default and an UNguarded one has to be
 * written deliberately, named in the table below, and justified.
 *
 * WHY THE GATE BOOLEANS CANNOT DO THIS JOB. scsd_sysap_vc_loss() clears
 * ps->dir_connected / ps->connected / ps->joiner_connected on a break, but two
 * of the three are read NEGATED by the dispatch (`!ps->dir_connected`,
 * `!ps->joiner_connected`), so clearing them RE-ARMS those sends. The durable
 * fact is the Path Block: breaking a circuit does not deallocate it (scs_vc.h
 * note 4), it leaves it CLOSED, and CLOSED is what this predicate reads.
 *
 * ============================ SEND SITE TABLE ============================
 * EVERY sender in this file and its verdict. ALL THREE parts of this table are
 * MECHANICALLY CHECKED by tests/vmsscs/test_scsd_send_sites.py, which parses
 * scsd.c and attributes every call to its enclosing function:
 *
 *   - it reds unless the ONLY function NAMING a transmit primitive is
 *     send_frame_raw(), and it names exactly one. The list is
 *     TRANSMIT_PRIMITIVES in that script (sendto, sendmsg, sendmmsg, send,
 *     writev, pwritev, pwrite, write, syscall) and it is matched as a BARE
 *     IDENTIFIER, so taking one as a value rather than calling it is caught
 *     too;
 *   - it reds unless the direct send_frame_raw() callers are exactly the EXEMPT
 *     list, with exactly the pinned per-function call counts;
 *   - it reds unless the send_frame_vc() callers are exactly the CHOKED list;
 *   - it reds unless the send_frame_channel() callers, and how many calls each
 *     makes, are exactly what the EXEMPT entry for it below says. The exemption
 *     is per FUNCTION, so its call sites are the one thing a name-only check
 *     could not see growing.
 *
 * Adding OR renaming a sender without editing this table reds the test run.
 *
 * THE PRIMITIVE CHECK IS WHY 'EVERY' IS SUPPORTABLE. Before vms-abc's second
 * round the census keyed only on the two wrapper NAMES, so a raw sendto() was
 * invisible to it by construction -- and MEASURED, one existed: main()'s HELLO
 * beacon loop called sendto() on the AF_PACKET socket directly, incremented
 * rx.hello_sent and appeared in the exit summary, while sitting in neither half
 * of a table that claimed to list every sender. It is now routed through
 * send_frame_channel() (see the EXEMPT entry), and the census keys on the
 * primitive so the next one cannot hide the same way.
 *
 *   CHOKED (go through send_frame_vc, refused on a non-OPEN circuit) -- these
 *   are SCS sequenced messages and connection-control messages, i.e. exactly
 *   the traffic p. 2-31 says a circuit carries:
 *     cm_send_config_burst()       op 0x14 / 0x01 / 0x02 add-member config
 *     scsd_handle_frame()          0x48 credit-return
 *                                  SCS$DIR_LOOKUP response
 *     scsd_sysap_msg_input()       0x81 CM transaction response
 *
 *   vms-7c0 MOVED that last one, and moved NOTHING ELSE. The 0x81 response used
 *   to be emitted from an inline block in scsd_handle_frame(); the block is now
 *   the VMS$VAXcluster SYSAP's p. 2-29 message input routine, reached through
 *   the CDL instead of by comparing Con.IDs against macros. Same builder, same
 *   bytes, same send_frame_vc() choke point, different enclosing function --
 *   which is exactly the change this table exists to make visible. No send was
 *   added, removed or re-routed by that item.
 *
 *   CHOKED, and new in vms-561: the three SERVICE EMITTERS. These are the
 *   port-driver half of the five SCS services (p. 2-56) -- scs_connect() and
 *   scs_accept() call them once per transition that names a packet. The frames
 *   are the same frames, from the same builders, in the same order; what
 *   changed is that the CDT and the state transition now belong to
 *   src/vmsscs/scs_svc.c instead of being open-coded around each send:
 *     scsd_svc_emit_connect_req()    0x4b joiner CONNECT-REQUEST (CONNECT), and
 *                                    the DEAD scsd_retransmit_tick() branch's
 *                                    0x4b retransmit, which reuses it
 *     scsd_svc_emit_dir_accept()     op=1 SCS$DIRECTORY CONNECT-ECHO and
 *                                    op=2 SCS$DIRECTORY CONNECT-RESPONSE (ACCEPT)
 *     scsd_svc_emit_member_accept()  0x4b VMS$VAXcluster CONNECT-RESPONSE (ACCEPT)
 *
 *   CHOKED, and new in vms-591: the DISCONNECT emitter. One function, two
 *   frames, both of the Figure 2-16 teardown arrows:
 *     scsd_svc_emit_disconnect()   the 62-byte DISCONNECT_REQ (message type 6)
 *                                  and the 58-byte DISCONNECT_RSP (type 7),
 *                                  built by src/vmsscs/scs_disc.c from
 *                                  byte-exact captured templates. Reached from
 *                                  scs_svc_deliver() on a received
 *                                  DISCONNECT_REQ/RSP, from scs_disconnect()
 *                                  when the SYSAP invokes its own symmetric
 *                                  disconnect, and from
 *                                  scs_svc_disconnect_all() at shutdown, and
 *                                  (vms-66f round 4) from scsd_poll_emit() when
 *                                  the Process Poller closes its p. 2-50 cycle
 *                                  -- four call paths, ONE send site, which is
 *                                  the shape this table exists to keep.
 *                                  CHOKED because a teardown is ordinary
 *                                  sequenced traffic: p. 2-31's rule applies
 *                                  and, if the circuit is already gone, every
 *                                  connection on it has been driven to CLOSED
 *                                  by scs_vc_break() and there is nothing left
 *                                  to disconnect.
 *
 *   CHOKED, and new in vms-7fe:
 *     scsd_send_sdir_refusal()     the 66-byte CONNECT_RSP that DECLINES a
 *                                  connect request -- p. 2-48 "no such SYSAP",
 *                                  and p. 2-50 "busy ... try again later" as a
 *                                  status value it will take but that THIS FILE
 *                                  NEVER PASSES IT (see the function). Not
 *                                  a service emitter, because the p. 2-48 scan
 *                                  failed before any SYSAP saw the request, so
 *                                  there is no CDT and no Figure 2-14
 *                                  transition; see the function for the full
 *                                  argument and for why the census's check 7
 *                                  lists it beside the three. It is never
 *                                  emitted in the configuration OVMX runs.
 *
 *   CHOKED, and new in vms-66f: the SCS PROCESS POLLER'S TWO SENDERS. These are
 *   the first frames OVMX has ever sent to a REMOTE SCS$DIRECTORY (everything
 *   above answers one). Both are sequenced messages on an established circuit,
 *   so both are choked like any other; neither takes an exemption:
 *     scsd_poll_emit()      the 110-byte 0x5b SCS$DIRECTORY CONNECT-REQUEST
 *                           the poller opens its cycle with (p. 2-50, template
 *                           SCA#21). This is the CONNECT service's emitter,
 *                           called by scs_connect() exactly as the three above.
 *                           Its DISCONNECT_REQ arm (round 4) does NOT build a
 *                           second teardown frame -- it delegates to
 *                           scsd_svc_emit_disconnect() above, so the p. 2-50
 *                           cycle-closing DISCONNECT_REQ is a FOURTH call path
 *                           into that one send site rather than a new one. It
 *                           still answers NOBUILDER for ACCEPT_RSP, which OVMX
 *                           genuinely cannot build.
 *     scsd_poll_inquire()   the 94-byte lookup REQUEST ("is <SYSAP> in your list
 *                           of listening SYSAPs?", template SCA#29). Not a
 *                           service emitter -- an inquiry is a SYSAP message on
 *                           an open connection, not a Figure 2-14 action -- so
 *                           it is listed separately, like scsd_send_sdir_refusal
 *                           above and for the same kind of reason.
 *
 *   CHOKED, and new in vms-578: THE SIXTEEN worktree-760 SENDERS (seventeen
 *   when merged; scs_send_disconnect() was deleted by vms-096, see below). These are
 *   the connection-manager / join-sequencer / pure-server-disk-client half of
 *   the daemon, merged in from worktree-760-active-directory. On that branch
 *   every one of them called send_frame_to() -- a SECOND raw sendto() wrapper,
 *   invisible to a name-keyed census and the exact hole the primitive check was
 *   added to close. send_frame_to is DELETED and all sixteen are CHOKED. None
 *   takes an exemption, and the argument is one argument for all of them: each
 *   is a SEQUENCED SCS MESSAGE addressed to a Path Block's remote port address
 *   and carrying a Con.ID pair, i.e. precisely the traffic p. 2-31 says a
 *   circuit carries. Not one of them is a formation packet, so refusing it on a
 *   non-OPEN circuit deadlocks nothing: the circuit these ride is opened by
 *   scsd_vc_emit() (EXEMPT, below) long before any of them runs.
 *
 *   THE JOIN SEQUENCER'S CLIENT HALF -- OVMX opening its own connections to the
 *   member, in the strict lookup-gated order of the join choreography:
 *     send_own_dir_connect_request()  OUR SCS$DIRECTORY CONNECT-REQUEST (step 1)
 *     send_own_dir_confirm()          its op=3 CONNECT-CONFIRM
 *     send_own_dir_lookup()           the SYSAP lookups that gate each step
 *     send_mscp_connect_request()     OUR MSCP$DISK client CONNECT-REQUEST
 *
 *   THE PURE-SERVER DISK CLIENT -- the same shape again on the DISTINCT PS
 *   Con.IDs, so the two never see each other's frames:
 *     ps_send_dir_connect()   ps_send_dir_confirm()   ps_send_dir_lookup()
 *     ps_send_mscp_connect()  ps_send_mscp_confirm()
 *     ps_send_scc()           SET CONTROLLER CHARACTERISTICS
 *     ps_send_gus()           GET UNIT STATUS
 *     ps_mscp_disc()          the discovery walk's 2 sends (SCC then GUS)
 *
 *   THE CONNECTION MANAGER:
 *     cm_send_ack()           the cat-0x04 ack that names the watermark
 *     cm_send_barrier_step()  the sec 4(p) barrier step request
 *     scs_reflect_credit()    the op8->op9 / op6->op7 credit handshake reply
 *     scs_send_disconnect_self() self-directed teardown, hand-built frame
 *
 *   CHOKED, and new in vms-46f: the REJOIN CREDIT-FIRST emitter.
 *     scs_send_rejoin_credit_op6() OVMX's op-6 special-credit request on the
 *                                  coordinator's SCS$DIRECTORY connection,
 *                                  emitted AHEAD of the deferred op 0x02 on a
 *                                  rejoin so the coordinator's Send Credit is
 *                                  replenished (spec 4(O.17), Davis pp.
 *                                  2-43/2-44). The frame is OVMX's own grounded
 *                                  76-byte op-6 (scs_dir_disc_tmpl); it is a
 *                                  sequenced SCS message on the OPEN directory
 *                                  circuit, choked like the sixteen above.
 *
 *   SIXTEEN, NOT SEVENTEEN (vms-096). scs_send_disconnect() was listed here and
 *   is DELETED: its only call site was a `cm_op == 6` block nested inside
 *   `cm_op == 8`, so it was unreachable and no build of this branch ever emitted
 *   its frame. Answering an op 6 -- the DISCONNECT_REQUEST of spec sec 4(h)(1a)
 *   -- is scs_disc_build_response()'s job, driven off the CDT by the vms-dd5
 *   classifier, and that path IS live and tested.
 *
 *   CHOKED, and new in vms-34b:
 *     scsd_mscp_srv_msg_input()  the MSCP disk-server responder's end message,
 *                                answering a command a real class driver sent
 *                                on an OPEN circuit -- the same argument as the
 *                                sixteen above: this is ordinary sequenced SCS
 *                                traffic on a connection that cannot exist
 *                                before its circuit does.
 *
 *   CHOKED, and new in vms-164d (DLM rung-1b):
 *     scsd_dlm_send_enq()        node-A's cross-node $ENQ request, an MTYPE-10
 *                                SYSAP message on the OPEN VMS$VAXcluster VC to
 *                                the peer's OVMX$DLM server handle -- ordinary
 *                                sequenced SCS traffic on a connection that
 *                                cannot exist before its circuit does.
 *     scsd_dlm_srv_msg_input()   node-B's GRANT response leg: the honest
 *                                executive status (a granted cross-node $ENQ,
 *                                a queued-reply, or -- new in vms-6ca -- the
 *                                DEFERRED GRANT the master WIREs off a real
 *                                $DEQ when a queued cross-node request finally
 *                                grants; never a fabricated grant) sent back on
 *                                the SAME OPEN VC, same argument as the MSCP
 *                                responder.
 *
 *   CHOKED, and new in vms-6ca (DLM rung H5):
 *     scsd_dlm_client_send_op()  node-A's generic DLM client SEND: the second,
 *                                contending $ENQ and the holder's $DEQ that drive
 *                                the block-then-grant-over-the-wire sequence.
 *                                MTYPE-10 SYSAP messages on the OPEN VMS$VAXcluster
 *                                VC to the peer's OVMX$DLM server handle -- ordinary
 *                                sequenced SCS traffic on a connection that cannot
 *                                exist before its circuit does, exactly like
 *                                scsd_dlm_send_enq above.
 *
 *   CHOKED, and new in vms-d81 (DLM rung H8):
 *     scsd_dlm_client_send_deq_valblk()  node-A's cross-node $DEQ that WRITES the
 *                                16-byte LVB. Same MTYPE-10 SYSAP message on the
 *                                OPEN VMS$VAXcluster VC to the peer's OVMX$DLM
 *                                server handle as scsd_dlm_client_send_op above,
 *                                but it carries the value block with LCK_M_VALBLK
 *                                set so the master replicates the wire value into
 *                                res->valblk (vms_lock_dlm_xnode_deq). Ordinary
 *                                sequenced SCS traffic on an already-open circuit;
 *                                choked like every other DLM client send.
 *
 *   CHOKED, and new in vms-dca9 (DLM rung H10b):
 *     scsd_dlm_client_send_rebuild()  node-A's REMASTER LOCK REBUILD send: a
 *                                TARGETED SCS_DLM_OP_REBUILD to a peer resolved
 *                                by CSID (peer_by_csid), not the peer whose own
 *                                per-peer join-tick context drove the call --
 *                                the resource's NEW directory master after its
 *                                old master's departure. Same MTYPE-10 SYSAP
 *                                framing and DLM-server addressing as every
 *                                sibling DLM client send above; ordinary
 *                                sequenced SCS traffic on a connection that
 *                                cannot exist before its circuit does.
 *
 *   CHOKED, and new in vms-ec75 (DLM rung H11): the TWO distributed-deadlock-search
 *   senders. Both are MTYPE-10 SYSAP messages on the OPEN VMS$VAXcluster VC to a
 *   peer resolved by CSID (peer_by_csid), ordinary sequenced SCS traffic like every
 *   sibling DLM send above:
 *     scsd_dlm_send_dlksrch()    the DLKSRCH PROBE: a SEARCH-HOLDER / SEARCH-RESOURCE
 *                                (and, in the multi-hop victim case, VICTIM) leg of
 *                                the edge-chase, forwarded home->master->home along
 *                                the cross-node wait-for edges to detect the cycle.
 *     scsd_dlm_send_victim_grant()  the VICTIM SIGNAL: an unprompted
 *                                SCS_DLM_OP_GRANT carrying status=SS$_DEADLOCK to the
 *                                victim node's DLM client, completing its queued
 *                                cross-node $ENQ with the deadlock abort (structurally
 *                                the H5 deferred GRANT, deadlock status).
 *
 *   CHOKED, and new in vms-600:
 *     scsd_mscp_srv_xfer()       the live scs_mscp_srv_xfer_fn: the SCA
 *                                block-transfer frames a READ streams before
 *                                its end message, addressed with the SAME
 *                                Con.ID pair on the SAME OPEN circuit as that
 *                                end message -- exactly the traffic p. 2-31
 *                                describes, called synchronously from inside
 *                                scs_mscp_srv_handle() while
 *                                scsd_mscp_srv_msg_input() (above) is still
 *                                on the stack.
 *
 *   CHOKED, and new in vms-ab1 (spec 4(O.29)):
 *     scsd_emit_clean_departure() the class-0x04 op-0x0d SELF-DEPARTURE CM open
 *                                (scs_member_build_depart), one per member on the
 *                                OPEN VMS$VAXcluster VC at shutdown, so the
 *                                coordinator REMOVES OVMX's CSB immediately
 *                                rather than holding it in reconnect/long_break.
 *                                A sequenced CM message on an established circuit,
 *                                choked like the CM senders above; if the VC is
 *                                already gone the peer learns of the departure
 *                                from the break, so refusal is correct.
 *
 *   NOT CLAIMED: that all sixteen have been exercised on a wire since the
 *   move. What IS true and mechanically checked is that none of them can reach
 *   the socket except through send_frame_vc(), because scsd.c contains exactly
 *   one transmit primitive and this census counts it.
 *
 *   EXEMPT (call send_frame_raw directly). Two functions, and the reason is the
 *   same in both: THESE FRAMES ARE HOW A CIRCUIT COMES TO EXIST. Refusing them
 *   on a non-OPEN circuit is not conservative, it is a deadlock -- no circuit
 *   could ever open, and a broken one could never re-form.
 *     scsd_vc_emit()        The 0x41 VC-FORMATION frames (START / STACK / ACK,
 *                           p. 2-14). The formation machine's states are
 *                           CLOSED, START SENT, START RECEIVED, OPEN; three of
 *                           the four are not OPEN, and the machine's whole job
 *                           is to transmit from them. The circuit state IS the
 *                           guard here -- scs_vc_fsm_*() decides what may be
 *                           sent from each state, which is a stricter check
 *                           than "is it OPEN", not a weaker one.
 *     send_frame_channel()  ALL NISCA HELLO traffic, called from 6 sites in 3
 *                           functions -- a set and a count BOTH PINNED by the
 *                           census (CHANNEL_CALLERS), so this paragraph
 *                           re-derives:
 *                             scsd_handle_frame()       4 -- the padded-probe
 *                               b4 ack, the rate-limited directed reply, the
 *                               one-shot proactive padded HELLO, and (vms-f3e)
 *                               the OVMX_MCAST_SOLICIT member-first directed
 *                               HELLO -- a heard MULTICAST HELLO answered with a
 *                               soliciting directed HELLO (abs-30 b2/PFW_INIT),
 *                               kill-switch-gated (default OFF), so it rides the
 *                               HELLO exemption exactly like the directed reply
 *                               beside it: NISCA channel traffic below any VC;
 *                             scsd_hello_beacon_emit()  1 -- the periodic
 *                               MULTICAST beacon off main()'s timer, the one
 *                               that increments rx.hello_sent and is reported
 *                               in the exit summary;
 *                             scsd_emit_port_lastgasp() 1 -- the vms-708 (spec
 *                               4(O.30)) PORT-LEVEL clean-leave last gasp, a
 *                               single final MULTICAST HELLO at shutdown (abs-30
 *                               b1 + cluster nonce, Davis p.7-29). Rides the
 *                               HELLO exemption for the same reason the beacon
 *                               does: addressed to the cluster multicast, no VC.
 *                           See that function for the reason; in short, HELLO
 *                           is CHANNEL maintenance BELOW the virtual circuit
 *                           and is the only thing that re-establishes a
 *                           channel after a break. The beacon is the strongest
 *                           case in the class: it is addressed to the cluster
 *                           MULTICAST group, not to any port address, and it is
 *                           sent when NO peer and therefore no Path Block and
 *                           no circuit is known at all -- it is how peers are
 *                           discovered. Gating it on an OPEN circuit would mean
 *                           a node could never announce itself.
 *
 *   THE EXEMPTION IS PER FUNCTION, WHICH IS WHY send_frame_channel() EXISTS.
 *   The three HELLO sends used to be inline in scsd_handle_frame(), so the
 *   census had to exempt that whole 1000-line function -- and MEASURED, four
 *   unguarded SCS senders inside it passed the census while transmitting on a
 *   CLOSED circuit. Never exempt a function that contains anything else.
 * =========================================================================
 *
 * scsd_refuse_without_open_vc - the predicate. Returns 1 (caller must send
 * nothing) when `vc` -- the circuit the caller intends to send on -- is not
 * OPEN; returns 0 when it is, filling *out (when non-NULL) with the CONFIG_PATH
 * view the caller then addresses the frame from.
 *
 * THE CIRCUIT IS NAMED, NOT ASSUMED. Every caller but one passes ps->pb, the
 * peer's own Path Block. send_joiner_connect_request() passes the circuit
 * CONNECT selected, which p. 2-47 permits to be a NAMED path or one found by
 * the CONFIG_SYS + OPEN scan -- not necessarily ps->pb. Reading ps->pb there
 * would check a different circuit from the one the frame rides.
 *
 * The state is read through scs_config_path() -- CONFIG_PATH (p. 2-47) -- not
 * by reaching into the Path Block, so the daemon sees the same view of a path
 * that any other CONFIG_PATH caller would. A NULL circuit, or one CONFIG_PATH
 * refuses to describe, is also "no circuit" and is refused the same way.
 *
 * NEVER SILENT, but never repetitive either: a dropped reply that says nothing
 * reads as a healthy run (CLAUDE.md rule 9 / INV-6), while one line per refused
 * frame buries the run -- the daemon re-enters these paths at the peer's HELLO
 * cadence, ~1 Hz, for the rest of the run. So the FIRST refusal on a circuit
 * logs SCSD-E-NOVC naming the circuit, its state and what was suppressed; later
 * refusals on the same circuit are counted, not printed, and the total is in
 * the exit summary. scsd_vc_on_open() clears the latch, so a circuit that
 * breaks a second time announces it a second time.
 */
static int scsd_refuse_without_open_vc(struct peer_state *ps, const struct scs_pb *vc,
                                       const char *what,
                                       struct scs_config_path_info *out)
{
    struct scs_config_path_info scratch;
    struct scs_config_path_info *path = (out != NULL) ? out : &scratch;
    if (vc != NULL && scs_config_path(vc, path) && path->vc_state == SCS_VC_OPEN) {
        return 0;
    }
    vc_sends_refused++;
    if (ps != NULL && ps->novc_logged) {
        return 1; /* already announced for this circuit -- counted, not printed */
    }
    if (ps != NULL) {
        ps->novc_logged = 1;
    }
    const uint8_t *pa = ps_port_addr(ps);
    log_ts(stderr);
    fprintf(stderr,
            " SCSD-E-NOVC, no OPEN virtual circuit to peer"
            " %02x:%02x:%02x:%02x:%02x:%02x (circuit is %s) -- %s NOT sent"
            " (p. 2-31: a send with no virtual circuit fails). Further refusals"
            " on this circuit are counted, not logged.\n",
            pa[0], pa[1], pa[2], pa[3], pa[4], pa[5],
            vc != NULL ? scs_vc_state_name(vc->vc_state) : "absent", what);
    fflush(stderr);
    return 1;
}

/*
 * SCSD_CREDIT_MAX_FRAME - the stamping scratch buffer. The largest frame any
 * scsd sender hands to send_frame_vc() is the 1500-byte padded probe; a frame
 * longer than this is transmitted UNSTAMPED rather than truncated, and the
 * refusal is counted, never silent.
 */
#define SCSD_CREDIT_MAX_FRAME 1518

/*
 * scsd_credit_stamp_outbound - the p. 2-43/2-44 SEND half (vms-aa1). See the
 * flow-control block above for which message types are eligible and why.
 *
 * Returns the frame to transmit: `scratch` (a stamped COPY) when this frame is
 * an eligible SYSAP message on a live connection with Send Credit, otherwise
 * `frame` itself, byte for byte. It NEVER refuses a send. p. 2-45's answer to
 * starvation is a Credit Wait, and there is no CDRP to queue here -- the frame
 * is already built and its sequence number already consumed -- so the honest
 * behaviour is to transmit the builder's bytes and COUNT the starvation
 * (credit_send_starved) rather than either drop a real frame or pretend a
 * credit existed. That counter is the p. 2-45 backlog this daemon cannot yet
 * express, printed in the exit summary so it is a measurement and not a gap.
 *
 * WHICH CONNECTION A FRAME BELONGS TO comes out of the frame itself: p. 2-35
 * makes SCA [54:58] the SENDER's own local Con.ID, which is exactly the CDL
 * index (p. 2-29). No caller passes a CDT and none needs to.
 */
static const uint8_t *scsd_credit_stamp_outbound(const uint8_t *frame, size_t len,
                                                 uint8_t *scratch)
{
    if (!scs_credit_enabled() || !scsd_cdl_ready || len <= 14) {
        return frame;
    }
    struct scs_rx_hdr h;
    if (scs_rx_parse(frame + 14, len - 14, &h) != 0) {
        return frame; /* not an envelope-conformant SCS message */
    }
    if (h.kind != SCS_RX_APP_MESSAGE) {
        return frame; /* MTYPE != 10 -- the credit field is not a piggyback */
    }
    if (len > SCSD_CREDIT_MAX_FRAME) {
        return frame; /* longer than the scratch */
    }
    /* vms-a61: NO scs_credit_header_offset() PRE-GATE HERE ANY MORE. `h`
     * already proves envelope conformance, which is what fixes the credit
     * field at content[48:50] regardless of length (scs_env.h) -- the length
     * allowlist that function re-derives is a redundant, narrower re-check of
     * a fact this frame already established. scs_credit_stamp_header() below
     * still does its own internal length lookup (it is a general primitive,
     * used elsewhere without a pre-proven envelope) and its failure is still
     * handled -- so removing the redundant pre-check here does not weaken
     * anything, it just stops re-deriving what `h` already proved. */
    struct scs_cdt *cdt = scs_cdl_lookup(&scsd_cdl, h.src_conid);
    if (cdt == NULL) {
        credit_send_no_cdt++;
        return frame;
    }
    int credit = scs_credit_on_send(cdt);
    if (credit < 0) {
        credit_send_starved++;
        return frame;
    }
    memcpy(scratch, frame, len);
    if (scs_credit_stamp_header(scratch + 14, h.total_sca_len, (unsigned)credit) != 0) {
        return frame;
    }
    credit_send_stamped++;
    credit_send_units += (unsigned long)credit;
    return scratch;
}

/*
 * send_frame_vc - THE CHOKE POINT. Transmit `frame` on circuit `vc`, or refuse.
 * Returns the byte count send_frame_raw() returned, or -1 if the circuit
 * refused it -- so every existing `... > 0` call-site idiom keeps working
 * unchanged and a refused send takes the same "nothing was sent" branch a
 * builder failure would.
 *
 * IT IS ALSO WHERE FLOW CONTROL IS DEBITED (vms-aa1). p. 2-43 debits a Send
 * Credit when the message is SENT, so the debit belongs at the transmit choke
 * point and not in any builder -- a frame that the circuit refuses must not
 * cost a credit, and a sender added later must be accounted without being
 * edited. The stamping is a pure function of the frame's own bytes; see
 * scsd_credit_stamp_outbound() above.
 *
 * THE DESTINATION COMES FROM THE CIRCUIT, not from the caller. p. 2-47: the
 * remote port address a frame is addressed to is a property of the Path Block
 * CONNECT selected, so it is read back out of the CONFIG_PATH view that just
 * proved the circuit OPEN. For every caller passing ps->pb this is byte-for-
 * byte ps_port_addr(ps), the value those sites passed before; for
 * send_joiner_connect_request() it is path.remote_port_addr, the value that
 * site already used.
 */
static ssize_t send_frame_vc(int sock, int ifindex, struct peer_state *ps,
                             const struct scs_pb *vc, const char *what,
                             const uint8_t *frame, size_t len)
{
    struct scs_config_path_info path;
    if (scsd_refuse_without_open_vc(ps, vc, what, &path)) {
        return -1;
    }
    uint8_t scratch[SCSD_CREDIT_MAX_FRAME];
    const uint8_t *out = scsd_credit_stamp_outbound(frame, len, scratch);
    return send_frame_raw(sock, ifindex, path.remote_port_addr, out, len);
}

/*
 * send_frame_channel - THE OTHER EXEMPTION, given a name so it can be counted.
 *
 * NISCA channel traffic: the directed and padded HELLO replies, and (since
 * vms-abc's second round) the periodic multicast HELLO beacon in main().
 * Deliberately NOT routed through send_frame_vc(), for the reason in the SEND
 * SITE TABLE -- a HELLO is not a sequenced message, carries no Con.ID, is
 * addressed to the frame's source MAC (or, for the beacon, to the cluster
 * multicast group) rather than to a Path Block's remote port address, is
 * exchanged with peers that have no Path Block at all, and is the only thing
 * that re-establishes a channel after a break. Gating it on an OPEN circuit
 * would make a broken circuit permanently unrecoverable, and would stop a
 * node announcing itself at all.
 *
 * IT IS ALSO WHERE THE TRANSMIT PRIMITIVE STOPS BEING SPELLED OUT TWICE. The
 * beacon used to build its own `struct sockaddr_ll` and call sendto() on the
 * socket directly, so the "a HELLO has no circuit" exemption was argued in two
 * places and enumerated in neither. It is argued once now, here.
 *
 * WHY IT IS A FUNCTION RATHER THAN THREE INLINE send_frame_raw() CALLS, which
 * is what it was: the census in tests/vmsscs/test_scsd_send_sites.py works by
 * attributing each call to its enclosing function. With the HELLO sends inline
 * in scsd_handle_frame(), exempting the HELLOs meant exempting that entire
 * 1000-line function -- and MEASURED, four unguarded SCS senders inside it
 * (the SCS$DIR_LOOKUP reply, the two SCS$DIRECTORY replies and the 0x81 CM
 * response) passed the census while transmitting on a CLOSED circuit. Hoisting
 * the three HELLO sends out is what shrinks the exemption to the frames it is
 * actually about.
 */
static ssize_t send_frame_channel(int sock, int ifindex, const uint8_t dst[6],
                                  const uint8_t *frame, size_t len)
{
    return send_frame_raw(sock, ifindex, dst, frame, len);
}

/*
 * ===== vms-4071: the VC FORMATION state machine, driven onto the NISCA wire =====
 *
 * The machine itself is in src/vmsscs/scs_vc.c (CLOSED / START SENT / START
 * RECEIVED / OPEN, the p. 2-14 acceptable-response table, the reissue timer,
 * the OS-dependent retry limit, the p. 2-16 implied ACK). What lives HERE is
 * only the translation between the machine's abstract packet classes and the
 * concrete 0x41 config-round frames the existing vms-21e builders emit:
 *
 *     SCS_VC_ACT_SEND_START -> scs_start_build,     config-round 0 (106 bytes)
 *     SCS_VC_ACT_SEND_STACK -> scs_start_build,     config-round 1 (106 bytes)
 *     SCS_VC_ACT_SEND_ACK   -> scs_start_build_ack, config-round 2 ( 46 bytes)
 *
 * No frame layout is re-derived: these are the same two builders, called with
 * the same parameters, that shipped before this item.
 *
 * ===== WHAT THE DEFAULT BUILD PUTS ON A FRESH-JOIN WIRE =====
 * Unchanged from pre-vms-4071, in BYTES and in ORDER RELATIVE TO THE PEER's
 * frames. The fresh-join interleaving is still
 *     OVMX r0, OVMX r1, peer r1, peer r2, OVMX r2
 * because scsd_vc_settle() holds OVMX's round-2 ack until the PEER's round-2
 * frame arrives, exactly as the pre-vms-4071 `else if (sv.is_ack)` branch did.
 * The state machine reaches OPEN one frame earlier than the ack goes out; that
 * is a state change, not a wire change. See scsd_vc_settle().
 *
 * MEASURED, not asserted (lab-2 pod vaxlab-3, 2026-08-03). Head-to-head fresh
 * joins, this branch (tag W9A) vs the same commit's parent main (tag WAA), same
 * pod, same runner, captures at
 * /data/training/vax/k8s-labs/vaxlab-3/logs/d94-{W9A,WAA}.pcap:
 *   - Both emitted 3 OVMX 0x41 frames in the order round-0, round-1, round-2,
 *     with the peer's frames interleaved identically:
 *       peer r0, OVMX r0, OVMX r1, peer r1, peer r2, OVMX r2.
 *   - Byte-diffing the three OVMX frames pairwise, the ONLY differing absolute
 *     offsets are 28, 60 and 109 -- the SCSSYSTEMID low byte (1409 vs 1410) and
 *     one SCSNODE character (OVMXW9 vs OVMXWA). Zero protocol bytes differ.
 *   - Emission counters identical: START-SENT=2 START-ACK-SENT=1
 *     CONNECT-REQ-SENT=1 CREDIT-SENT=8 DIR-CONNECT-RESP-SENT=1
 *     DIR-LOOKUP-RESP-SENT=4 CM-CONFIG-FRAMES=3.
 * NEITHER run reached CLUSTER_NODES=3 on that pod, and that is PRE-EXISTING:
 * the main control failed identically. This item does not claim to fix it.
 *
 * ===== WHAT IS NEW ON THE WIRE, AND WHAT GATES IT =====
 *   1. RETRY / ABANDON. A START/STACK unanswered for
 *      SCS_VC_FORMATION_TIMEOUT_MS is reissued (p. 2-14) up to
 *      scs_vc_retry_limit() times, after which formation is ABANDONED. Fires
 *      only under loss; never on a clean join. Kill-switch:
 *      OVMX_VC_NO_RETRY_LIMIT=1 restores unbounded retry with no abandon.
 *   2. THE EARLY ACK -- OPT-IN, DEFAULT OFF. OVMX_VC_EARLY_ACK=1 emits the
 *      round-2 ack on the OPEN transition instead (the literal p. 2-14 rule:
 *      "A response of either ACK or STACK will advance the circuit to the OPEN
 *      state; and if the response is STACK, the port driver will issue an
 *      ACK"). Same 46 bytes, one peer-frame earlier. NO lab capture has shown
 *      the reference member accepts that ordering, so it does not ship on by
 *      default. See SCS_VC_EARLY_ACK_ENV in scs_vc.h.
 *
 *      KILL-SWITCH PROVEN TO MOVE (guardrail 23), lab-2 tag W5A vs W1A: with
 *      OVMX_VC_EARLY_ACK=1 the daemon logs SCSD-I-STARTDONE immediately after
 *      "STACK received -> send ACK, VC OPEN" and BEFORE the peer's round-2
 *      arrives; with the switch off it logs it after "ACK received", i.e. after
 *      the peer's round-2 -- which is where the pre-vms-4071 control (W4A)
 *      logs it too. NOTE HONESTLY: on THIS lab the two orderings are NOT
 *      distinguishable in the capture, because the peer emits its round-1 and
 *      round-2 back to back inside one millisecond, so both are already on the
 *      wire before OVMX can turn its ack around. The switch changes the daemon's
 *      emission point; on this peer's timing it does not change the pcap.
 *   3. THE IMPLIED ACK (p. 2-16) can open a circuit with no peer round-2 ever
 *      arriving, in which case OVMX owes its round-2 immediately. Reachable
 *      only from START RECEIVED on an SCS sequenced-message class, which a
 *      clean join never produces mid-dialogue.
 */
struct scsd_vc_ctx {
    int             sock;
    int             ifindex;
    const uint8_t  *hw_mac;      /* OVMX HW MAC (Ethernet src) */
    const uint8_t  *src_logical; /* OVMX cluster-logical addr (SCA abs 24) */
    uint16_t        scssystemid;
    const char     *node_name;
    struct scs_config *cfg;
};

/*
 * scsd_vc_params - fill in the 0x41 formation body for this peer.
 *
 * SPLIT OUT OF scsd_vc_emit() (vms-096) and it TRANSMITS NOTHING -- deliberately
 * so. scsd_vc_emit() is one of the two functions EXEMPT from the send_frame_vc()
 * choke point (see the SEND SITE TABLE), and tests/vmsscs/test_scsd_send_sites.py
 * caps an exempt function at 60 lines precisely so that an exemption granted for
 * three formation sends cannot creep into covering a page of unrelated logic.
 * Restoring the two vms-2f3 time quadwords pushed it to 63. The right answer is
 * the one that gate's failure message names: hoist, do not widen the exemption.
 */
static void scsd_vc_params(const struct scsd_vc_ctx *ctx, struct peer_state *ps,
                           struct scs_start_params *sp)
{
    memset(sp, 0, sizeof(*sp));
    memcpy(sp->dst_mac, ps_port_addr(ps), 6);
    memcpy(sp->src_mac, ctx->hw_mac, 6);
    memcpy(sp->src_logical, ctx->src_logical, 6);
    memcpy(sp->peer_logical, ps_sys_addr(ps), 6);
    sp->scssystemid = ctx->scssystemid;
    strncpy(sp->node_name, ctx->node_name, SCS_START_NODENAME_LEN);
    sp->node_name[SCS_START_NODENAME_LEN] = '\0';
    /* GROUNDED joiner values, unchanged from vms-21e: every joiner 0x41 frame
     * carries send_seq=1 / recv_ack=0 (spec sec 4i.A) and echoes the member's
     * advertised node-incarnation at [22:24] (spec sec 4i.B). */
    sp->send_seq = ps->vc.seq.send_seq;
    sp->recv_ack = 0;
    sp->incarnation = ps->incarnation ? ps->incarnation : 1;
    /* [66:74] OUR OWN incarnation -- a live per-boot VMS timestamp, and a
     * DIFFERENT field from the [22:24] counter above. See
     * ovmx_incarnation_time(): a frozen value here made every peer reuse the
     * System Block from our previous incarnation (vms-2f3, c302b7d).
     *
     * THIS IS THE ONLY PRODUCTION CALLER OF ovmx_incarnation_time(), and it
     * hangs off scsd_vc_emit() rather than the old main() site because the
     * vms-4071 VC-FSM refactor moved every 0x41 emission behind that function.
     * It was LOST in that move -- for a while OVMX shipped the REPLAYED
     * TEMPLATE quadword that scs_start.h calls the control arm, on every
     * START/STACK, with the suite green: test_scs_start.c covered
     * scs_start_build() only, never the daemon.
     * tests/vmsscs/test_scsd_wire.c's
     * test_vc_start_carries_a_live_per_boot_incarnation() now asserts the
     * DAEMON stamps it, so a second such move cannot be silent.
     *
     * AND IT IS PROVEN ON THE WIRE, not only in the suite: lab-2 vaxlab-4 run
     * V96 (2026-08-05) put 0x00bc087816c76364 -- 5-AUG-2026 15:32:29, the second
     * the daemon started -- in all four of its 0x41 frames and JOINED
     * (CLUSTER_NODES=3, XITDONE=1), where run B2 of the vms-578 acceptance
     * bracket, same pod, pre-fix binary, put the captured template's
     * 0x00bc00947a678ebb. See the matched pair in scs_start.h. */
    sp->incarnation_time = ovmx_incarnation_time();
    /* [98:106] when THIS frame was composed -- live per frame, not per boot.
     * Same env kill-switch shape as the incarnation so the two can be bisected
     * independently. */
    {
        const char *mt_frozen = getenv("OVMX_MSGTIME_FROZEN");
        sp->message_time = (mt_frozen != NULL && mt_frozen[0] == '1')
                           ? 0u : scs_member_vms_time_now();
    }
}

/*
 * scsd_vc_emit - transmit the formation packet the state machine asked for.
 * Returns 1 if a frame went out, 0 otherwise. `act` must be one of the three
 * SEND_* actions; anything else is a no-op.
 */
static int scsd_vc_emit(const struct scsd_vc_ctx *ctx, struct peer_state *ps,
                        enum scs_vc_action act)
{
    if (ctx == NULL || ps == NULL || ps->pb == NULL) {
        return 0;
    }
    struct scs_start_params sp;
    scsd_vc_params(ctx, ps, &sp);

    if (act == SCS_VC_ACT_SEND_ACK) {
        uint8_t aframe[SCS_START_ACK_FRAME_LEN];
        if (scs_start_build_ack(&sp, aframe) != 0) {
            return 0;
        }
        return send_frame_raw(ctx->sock, ctx->ifindex, ps_port_addr(ps),
                             aframe, sizeof(aframe)) > 0 ? 1 : 0;
    }
    if (act == SCS_VC_ACT_SEND_START || act == SCS_VC_ACT_SEND_STACK) {
        sp.config_round = (act == SCS_VC_ACT_SEND_START) ? 0 : 1;
        uint8_t sframe[SCS_START_FRAME_LEN];
        if (scs_start_build(&sp, sframe) != 0) {
            return 0;
        }
        return send_frame_raw(ctx->sock, ctx->ifindex, ps_port_addr(ps),
                             sframe, sizeof(sframe)) > 0 ? 1 : 0;
    }
    return 0;
}

/*
 * scsd_open_result_text - what the p. 2-21 open transition did, as one clause
 * for the log line. Purely a name for an enum value: it asserts nothing about
 * whether the daemon can reach it, because a log branch that claims a rule the
 * daemon cannot execute is dead code wearing a fact's clothes (that is what the
 * two "UNEXPECTED: believed unreachable" clauses here were before vms-17f).
 * Reachability is stated once, below, with the test that measures it.
 */
static const char *scsd_open_result_text(enum scs_open_result r)
{
    switch (r) {
    case SCS_OPEN_NEW_SB:
        return "node learned for the first time";
    case SCS_OPEN_EXISTING_REFRESHED:
        return "old system block REFRESHED (rejoin, p. 2-21 Note)";
    case SCS_OPEN_EXISTING_SB:
        return "queued to the existing system block";
    default:
        return "OPEN FAILED -- no path block or no system block";
    }
}

/*
 * scsd_vc_on_open - the p. 2-21 open transition, run ONCE per circuit whenever
 * the state machine reaches OPEN (by STACK, by a bare ACK, or by the p. 2-16
 * implied ACK). Body unchanged from vms-21e/vms-246/vms-7be; only its trigger
 * moved from "the peer sent its round-2 ack" to "the circuit reached OPEN".
 *
 * REACHABILITY AS MEASURED (vms-17f, extended by vms-22e), so a green test run
 * is not misread. Each line is a counter this function increments, and each is
 * asserted by a named case in tests/vmsscs/test_scsd_wire.c that drives
 * scsd_handle_frame() or scsd_vc_on_open() directly:
 *
 *   SCS_OPEN_NEW_SB              REACHED. Every peer discovered for the first
 *                                time. (test_vc_happy_path_frame_sequence and
 *                                the rejoin case below.)
 *   SCS_OPEN_EXISTING_REFRESHED  REACHED SINCE vms-17f, and only since: it needs
 *                                the departure sweep to have closed the previous
 *                                Path Block and released the peer slot, so that
 *                                the returning node builds a second, FORMATIVE
 *                                PB whose open finds the old SB with an empty PB
 *                                queue. (test_rejoin_reaches_the_p221_refresh.)
 *   SCS_OPEN_EXISTING_SB         NOT REACHED, and not reachable as OVMX is
 *                                built: it needs one node presenting TWO ports,
 *                                so that its SB still has another PB queued to
 *                                it when the second circuit opens. OVMX binds a
 *                                single interconnect and creates one PB per peer
 *                                MAC. Left in scsd_open_result_text() as a name,
 *                                claiming nothing.
 *   SCS_OPEN_ERROR               Only from a PB with no SB attached.
 *   SCS_OPEN_ABANDONED_MASQUERADE
 *                                NOT REACHED BY THE WIRE, and cannot be today:
 *                                the p. 2-21 footnote comparisons need BOTH
 *                                sides populated, and the formative side never
 *                                is -- struct scs_start_view carries no node
 *                                name and no incarnation, so the only thing
 *                                SCSD learns about a REMOTE node is its 48-bit
 *                                System Address (scs_pb_learn_system_addr),
 *                                leaving node_name empty and incarnation 0. All
 *                                three tests are therefore INDETERMINATE on the
 *                                live path and scs_config_masquerade_check()
 *                                returns SCS_MASQ_PASS. The branch below is NOT
 *                                dead code wearing a fact's clothes: it is
 *                                driven end-to-end by
 *                                test_masquerade_open_is_logged_and_suppresses_vcopen
 *                                in test_scsd_wire.c, which seeds a peer_state
 *                                whose SB does carry a name and calls this
 *                                function, and which asserts the SCSD-W-VCMASQ
 *                                text names the failing test AND that no
 *                                STARTDONE/VCOPEN line follows.
 */
static void scsd_vc_on_open(const struct scsd_vc_ctx *ctx, struct peer_state *ps)
{
    /* vms-246: the phase-2 config-round counters are SEPARATE from the SCS VC;
     * both sides reset the VC to send_seq=1/recv_seq=0 when START completes. */
    scs_vc_reset_seq(&ps->vc);
    enum scs_open_result open_res = scs_pb_open(ctx->cfg, ps->pb);
    if (open_res == SCS_OPEN_ABANDONED_MASQUERADE) {
        /* vms-22e: the p. 2-21 footnote tests rejected the formative System
         * Block. Log WHICH test failed and stop -- the circuit is not open, so
         * claiming STARTDONE/VCOPEN below would be a lie. The Path Block is left
         * abandoned; tearing it down is vms-17f's surface, not this function's.
         * NOTE this is unreached in production today. A comparison needs BOTH
         * sides populated, and the formative side never is: the only thing SCSD
         * learns about a REMOTE node is its 48-bit System Address
         * (scs_pb_learn_system_addr), with node_name empty and incarnation 0.
         * (SCSD does give its OWN System Block a node name, above in main(), but
         * that is the other side of the comparison and cannot supply the
         * missing one.) See the ANTI-MASQUERADE block in scs_config.h. */
        pb_open_masquerades++;
        log_ts(stderr);
        fprintf(stderr,
                " SCSD-W-VCMASQ, virtual-circuit formation ABANDONED with peer"
                " %02x:%02x:%02x:%02x:%02x:%02x -- masquerade test failed: %s"
                " (p. 2-21 footnote)\n",
                ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
                scs_masquerade_result_name(
                    (enum scs_masquerade_result)ps->pb->masquerade_fail));
        fflush(stderr);
        return;
    }
    if (open_res >= SCS_OPEN_NEW_SB && open_res <= SCS_OPEN_EXISTING_REFRESHED) {
        pb_open_results[(int)open_res]++;
    } else {
        pb_open_errors++;
    }
    /* vms-abc: the circuit carries traffic again, so the "this circuit refuses
     * sends" announcement is spent. Clearing it here (and ONLY here) is what
     * makes the latch per-BREAK rather than once-per-process: a circuit that
     * opens, breaks, re-opens and breaks again logs SCSD-E-NOVC twice. */
    ps->novc_logged = 0;
    log_ts(stdout);
    printf(" SCSD-I-STARTDONE, START/config complete with peer"
           " %02x:%02x:%02x:%02x:%02x:%02x -- VC reset"
           " (send_seq=%u recv_seq=%u), awaiting 0x4b connect\n",
           ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
           ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
           ps->vc.seq.send_seq, ps->vc.seq.recv_seq);
    printf(" SCSD-I-VCOPEN, path block OPEN, %s"
           " (configuration queue holds %u system blocks,"
           " %u path block(s) on this node's SB)\n",
           scsd_open_result_text(open_res),
           scs_config_sb_count(ctx->cfg),
           scs_sb_pb_count(ps->pb->sb));
    fflush(stdout);
}

/*
 * scsd_vc_ack_due - THE WIRE-ORDERING DECISION, isolated in one predicate.
 *
 * The circuit being OPEN is not by itself permission to transmit OVMX's round-2
 * ack; WHEN that 46-byte frame goes out is what decides whether a fresh join
 * looks byte-and-order identical to pre-vms-4071. Three ways it becomes due:
 *
 *  1. DEFAULT / PRESERVED (`peer_round2_seen`): the peer's own round-2 frame has
 *     arrived. This is precisely the pre-vms-4071 trigger -- the old code emitted
 *     the ack from the `else` branch gated on scs_start_view.is_ack. Keeping it
 *     as the default is what makes the fresh-join interleaving unchanged.
 *  2. OPT-IN (`OVMX_VC_EARLY_ACK=1`): emit as soon as the circuit is OPEN, i.e.
 *     on the peer's round-1 STACK. That is the literal p. 2-14 rule, but it puts
 *     the ack one peer-frame earlier and NO lab capture has yet shown the
 *     reference member accepts it, so it is off unless asked for.
 *  3. IMPLIED ACK (p. 2-16, `fsm.implied_acks`): the peer is already sending
 *     circuit traffic, so it considers the circuit OPEN and will never send a
 *     round-2 for case 1 to wait on. Waiting would deadlock the dialogue. This
 *     path is unreachable on a clean join (it needs an SCS sequenced-message
 *     class to arrive while the PB is in START RECEIVED).
 *
 * Case 3 is an OVMX design choice, labeled per rule 8: p. 2-16 says only that
 * the circuit is marked OPEN, not that anything is transmitted. It is the NISCA
 * encoding that requires both nodes to carry every config round (spec sec 4g
 * phase 2: 0/0/1/1/2/2, "both nodes carry the same round").
 */
/*
 * scsd_vc_peer_round2 - THE DEFAULT ack trigger, in one place so the daemon and
 * its test cannot drift apart about what it is: the peer's round-2 46-byte
 * frame, which scs_vc_classify_round() maps to SCS_VC_EV_ACK. This is exactly
 * the condition the pre-vms-4071 code branched on (scs_start_view.is_ack).
 */
static int scsd_vc_peer_round2(enum scs_vc_event ev)
{
    return ev == SCS_VC_EV_ACK;
}

/*
 * scsd_member_initiate_enabled - vms-d60 (rung-VC): is OVMX's member-role START
 * INITIATE armed? The member-role umbrella flag OVMX_MCAST_SOLICIT (which also
 * arms rung-0's directed-HELLO solicit), with OVMX_NO_START_INITIATE as the
 * suppression sibling (guardrail idiom). Read fresh -- these are lab switches,
 * not hot-path state. Absent the flag, OVMX never initiates and never takes the
 * simultaneous-START ack-due path below, so the OVMX<->VAX wire is unchanged.
 */
static int scsd_member_initiate_enabled(void)
{
    return getenv("OVMX_MCAST_SOLICIT") != NULL &&
           getenv("OVMX_NO_START_INITIATE") == NULL;
}

static int scsd_vc_ack_due(const struct peer_state *ps, int peer_round2_seen)
{
    if (peer_round2_seen) {
        return 1;
    }
    if (scs_vc_early_ack_enabled()) {
        return 1;
    }
    return ps->pb != NULL && ps->pb->fsm.implied_acks > 0;
}

/*
 * scsd_vc_settle - after any state-machine step, do the two things that are
 * NISCA-specific rather than SCA-abstract:
 *
 *  - If the circuit is OPEN, OVMX has not yet put its round-2 ack out, and
 *    scsd_vc_ack_due() says it is time, emit it and run the p. 2-21 open
 *    transition. `peer_round2_seen` is 1 only at the call site that just
 *    processed the peer's 46-byte round-2 frame.
 *  - If formation was ABANDONED, re-arm the Path Block's formation machine so a
 *    later START from the same peer can start the dialogue over. p. 2-14 says
 *    only that formation is abandoned, not what happens next; OVMX cannot tear
 *    the Path Block down here because the peer-slot/PB lifecycle is vms-17f, so
 *    it resets the machine in place and says so in the log.
 */
static void scsd_vc_settle(const struct scsd_vc_ctx *ctx, struct peer_state *ps,
                           enum scs_vc_action act, int peer_round2_seen,
                           long *start_ack_sent)
{
    if (ps->pb == NULL) {
        return;
    }
    if (act == SCS_VC_ACT_ABANDON) {
        log_ts(stderr);
        fprintf(stderr,
                " SCSD-W-VCABANDON, virtual-circuit formation ABANDONED with peer"
                " %02x:%02x:%02x:%02x:%02x:%02x after %lu reissue(s) (p. 2-14)"
                " -- path block re-armed to CLOSED for a fresh dialogue\n",
                ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
                ps->pb->fsm.reissues);
        fflush(stderr);
        scs_vc_fsm_init(ps->pb);
        ps->start_replied = 0;
        return;
    }
    /* vms-d60 (rung-VC): the SIMULTANEOUS-START ack-due path. When two OVMX
     * nodes both INITIATE round-0 START (member role), the FSM's Figure 2-7
     * collision resolution reaches OPEN a step EARLY -- via the peer's round-1
     * STACK (EV_STACK), returning SCS_VC_ACT_SEND_ACK -- NOT via the peer's
     * round-2 ACK the default trigger (scsd_vc_peer_round2 == EV_ACK) waits for.
     * That round-2 ACK never comes in the symmetric case, so start_acked never
     * latched and the sequencer never ignited (rd vms-f3e rung-VC stall: VC=OPEN
     * but start_acked=0). The FSM RETURNING SEND_ACK on the OPEN transition IS
     * the round-2-ack-due signal here -- OVMX's ACK is the round-2, with no peer
     * round-2 to order behind. GATED on the member-role flag so a real VAX
     * (flag off, OVMX only reflects, reaches OPEN via the member's EV_ACK) is
     * byte-identical. */
    int collision_ack_due = (act == SCS_VC_ACT_SEND_ACK) &&
                            scsd_member_initiate_enabled();
    if (ps->pb->vc_state == SCS_VC_OPEN && !ps->start_acked &&
        (scsd_vc_ack_due(ps, peer_round2_seen) || collision_ack_due)) {
        if (scsd_vc_emit(ctx, ps, SCS_VC_ACT_SEND_ACK)) {
            (*start_ack_sent)++;
            ps->start_acked = 1;
            scsd_vc_on_open(ctx, ps);
        }
    }
}

/*
 * cm_send_config_burst - vms-224: drive OVMX's connection-manager add-member
 * config burst on the bound 190-byte VMS$VAXcluster VC (spec sec 4j): op 0x14
 * (node model advertisement), op 0x01 (cluster parameters, VOTES=0 non-voting),
 * op 0x02 (config/topology). Each is a sequenced SCS message -- it advances the
 * VC send_seq (SCS layer) AND carries its own SYSAP send-msg# (application
 * layer). This is the joiner's active contribution that lets the member drive
 * the op 0x03 commit + op 0x05 lock-rebuild transactions, promoting OVMX to a
 * full MEMBER. Returns the number of frames sent (0..3).
 */
static int cm_send_config_burst(int sock, int ifindex, struct peer_state *ps,
                                const uint8_t our_hw_mac[6],
                                const uint8_t our_src_logical[6],
                                uint32_t local_conid, uint32_t remote_conid)
{
    if (ps->sysap_send == 0) {
        ps->sysap_send = 1; /* SYSAP send-msg# starts at 1 (spec sec 4j) */
    }

    struct scs_member_params mp;
    uint8_t frame[SCS_MEMBER_FRAME_LEN];
    int sent = 0;

    /* Shared identity + connection fields for all three frames. The Con.ID pair
     * identifies WHICH VMS$VAXcluster connection the burst rides (vms-d94: the
     * add-member burst must ride the JOINER-initiated connection). */
    memset(&mp, 0, sizeof(mp));
    memcpy(mp.dst_mac, ps_port_addr(ps), 6);
    memcpy(mp.src_mac, our_hw_mac, 6);
    memcpy(mp.src_logical, our_src_logical, 6); /* vms-9f3: abs-24 cluster-logical addr */
    memcpy(mp.peer_logical, ps_sys_addr(ps), 6);
    mp.remote_conid = remote_conid;
    mp.local_conid = local_conid;
    mp.incarnation = ps->incarnation;

    /* vms-760 server-first: match af2's initial add-member burst EXACTLY -- the
     * joiner sends only MODEL + PARAMS (2 frames), each with sysap_ack_msg=0 (NOT
     * acking the member's config, even though it arrived), and does NOT send the
     * op 0x02 CONFIG initially (af2 sends CONFIG ~1.8s later as part of the
     * member-driven reconfiguration round). Sending CONFIG early + acking amsg!=0
     * left VAX1 acking with a bare cat-0x04 and never entering reconfiguration. */
    /* vms-760: the joiner's INITIAL add-member burst is MODEL + PARAMS ONLY, with
     * sysap_ack_msg=0; op0x02 CONFIG is DEFERRED to the later reconfiguration round.
     * GROUNDED twice: af2 143.759 and the 2->3 reference (VAX3 ss=14/15 -> VAX1
     * reciprocates MODEL+PARAMS immediately). Sending CONFIG in the initial burst
     * leaves the member silent. Applies to BOTH joiner-driven paths. */
    int pure = (getenv("OVMX_PURE_SERVER") != NULL) ||
               (getenv("OVMX_JOIN_SEQ") != NULL);

    /* op 0x14 node CPU/model advertisement (OVMX's own model string). */
    mp.recv_ack = ps->vc.seq.recv_seq;
    mp.send_seq = scs_seq_advance(&ps->vc.seq);
    mp.sysap_send_msg = ps->sysap_send++;
    mp.sysap_ack_msg = pure ? 0 : ps->sysap_recv;
    mp.model = NULL; /* OVMX_MODEL_STRING default */
    if (scs_member_build_model(&mp, frame) == 0 &&
        send_frame_vc(sock, ifindex, ps, ps->pb,
                      "add-member op 0x14 (node model)", frame, sizeof(frame)) > 0) {
        sent++;
    }

    /* op 0x01 cluster parameters -- VOTES=0 (OVMX joins non-voting so it can
     * never break VAX1/VAX2 quorum, design sec 8). */
    mp.recv_ack = ps->vc.seq.recv_seq;
    mp.send_seq = scs_seq_advance(&ps->vc.seq);
    mp.sysap_send_msg = ps->sysap_send++;
    mp.sysap_ack_msg = pure ? 0 : ps->sysap_recv;
    mp.votes = SCS_MEMBER_VOTES_NONVOTING; /* VOTES=1 tested (vms-d94), did NOT change NEW->MEMBER */
    /* vms-e81: advertise the MEMBER form only once we actually are one AND we
     * have a member's own values to copy. Anything less stays the joiner form,
     * which is at least honest. */
    if (ovmx_cluster.admitted && ovmx_cluster.known) {
        mp.is_member = 1;
        mp.member_count = ovmx_cluster.member_count;
        mp.cluster_formed = ovmx_cluster.formed;
        mp.last_transition = ovmx_cluster.last_transition;
        mp.own_admission = ovmx_cluster.own_admission;
    }
    if (scs_member_build_params(&mp, frame) == 0 &&
        send_frame_vc(sock, ifindex, ps, ps->pb,
                      "add-member op 0x01 (cluster parameters)", frame, sizeof(frame)) > 0) {
        sent++;
    }

    /* op 0x02 config/topology. In pure-server (af2) mode this is DEFERRED to the
     * reconfiguration round; only the legacy active-joiner path sends it here.
     *
     * vms-578 INTEGRATION -- BOTH SIDES CHANGED THIS AND worktree-760 WINS.
     * work/vms-187-closure sent op 0x02 unconditionally in the initial burst,
     * citing vms-d94 ("holding 0x02 to a later step did NOT change
     * NEW->MEMBER"). That measurement predates the server-first path: on
     * worktree-760 the deferred send is paired with the SCSD-I-CMCONFIG2 retry,
     * and docs/HANDOFF-vms-2f3.md sec 3 item 7 records the retry as REQUIRED.
     * The unconditional send is what an established VAX1 answers with a bare
     * cat-0x04 instead of entering reconfiguration. The `pure` guard and the
     * deferred retry are therefore kept; the closure's version is dropped
     * DELIBERATELY, not lost. Send now goes through the choke point.
     *
     * vms-46f (spec 4(O.17)): on the REJOIN credit-first path the config MUST
     * also be deferred here so it does not ride AHEAD of the op-6 special-credit
     * request. The SUCCESS oracle's order is model(ss10), params(ss11),
     * op7(ss12), op6(ss13), op02(ss14) -- model+params precede op-6, but the
     * config (op02) follows it. Emitting the config in this burst would put an
     * op 0x02 ahead of op-6 (still credit-starved). It rides via the deferred
     * SCSD-I-CMCONFIG2 path, which the fix gates behind the op-6 latch. */
    if (!pure && !rejoin_credit_first_enabled()) {
        mp.recv_ack = ps->vc.seq.recv_seq;
        mp.send_seq = scs_seq_advance(&ps->vc.seq);
        mp.sysap_send_msg = ps->sysap_send++;
        mp.sysap_ack_msg = ps->sysap_recv;
        cm_apply_rejoin_form(&mp); /* vms-2f3 */
        if (scs_member_build_config(&mp, frame) == 0 &&
            send_frame_vc(sock, ifindex, ps, ps->pb,
                          "add-member op 0x02 (config/topology)", frame, sizeof(frame)) > 0) {
            sent++;
        }
    }

    ps->cm_config_sent = 1;
    ps->psc_gate_ms = monotonic_ms(); /* vms-2f3: disk-discovery ungate timeout base */
    /* vms-760: remember which VC carried it so the deferred op 0x02 rides the same one. */
    if (sent > 0) {
        ps->cfg_sent = 1;
        ps->cfg_ms = monotonic_ms();
        ps->cfg_local_conid = local_conid;
        ps->cfg_remote_conid = remote_conid;
    }
    return sent;
}

/* =====================================================================
 * vms-fdd -- SCA CONNECT DATA (VAXcluster Principles p. 2-25, p. 2-28)
 * =====================================================================
 *
 * p. 2-25: the initiating SYSAP supplies up to 16 bytes of connect data in
 * CONNECT_REQ and the target up to 16 in ACCEPT_REQ, and the two connection
 * managers "use this data to effectively identify to each other which version
 * of VMS each is associated with" -- either end may refuse on it. p. 2-28 puts
 * the field in the CDT. The wire location, the census that grounds the value
 * and the honest gap are the CONNECT DATA verdict in scs_connect.h.
 *
 * TWO SIDES, AND ONLY ONE OF THEM IS OURS:
 *   - OUTBOUND: scs_connect_build_request()/_response() stamp the bytes; the
 *     two helpers below record the same bytes on the CDT the CONNECT / ACCEPT
 *     service just allocated, which is what p. 2-28 asks for.
 *   - INBOUND: OVMX DECODES AND LOGS the peer's value on every CONNECT_REQ and
 *     ACCEPT_REQ it receives, and does NOT act on it. OVMX has no version
 *     policy: it never rejects a peer on connect data, because we have exactly
 *     one VMS version on the lab wire (V7.3) and no observation of what a
 *     refusal looks like. Logging it is the honest half; a version policy
 *     built on one data point would be an invented one.
 */
static void scsd_cdt_record_connect_data(struct scs_cdt *cdt)
{
    if (cdt == NULL || !scs_connect_data_enabled()) {
        return;
    }
    (void)scs_cdt_set_connect_data(cdt, scs_connect_data_vaxcluster,
                                   SCS_CONNECT_DATA_LEN);
}

/* Decode + log the peer's connect data. Returns 1 if the frame carried a
 * decodable field (i.e. it was a CONNECT_REQ or ACCEPT_REQ), 0 otherwise. */
static int scsd_log_peer_connect_data(const uint8_t *buf, size_t n)
{
    uint8_t cd[SCS_CONNECT_DATA_LEN];
    if (scs_connect_data_get(buf, n, cd) != 0) {
        return 0;
    }
    /* [46:48] = abs 60:62; scs_connect_data_get() already required it to be
     * one of these two, and the frame to be long enough to hold it. */
    uint16_t cmsg = (uint16_t)(buf[60] | ((uint16_t)buf[61] << 8));
    /* [62:78] = abs 76:92, the sender's local SYSAP name (spec sec 4h(2)).
     * Measured ASCII in 1425/1425 VAX-sourced connect frames (and in all 466
     * OVMX-sourced ones), but this is peer-supplied
     * and goes straight into a log line, so sanitize rather than trust it. */
    char sysap[17];
    for (int i = 0; i < 16; i++) {
        uint8_t c = buf[76 + i];
        sysap[i] = (c >= 32 && c < 127) ? (char)c : '.';
    }
    sysap[16] = '\0';
    char rendered[80];
    log_ts(stdout);
    printf(" SCSD-I-CONNDATA, peer %s connect data (p. 2-25) from"
           " %s: %s%s\n",
           cmsg == SCS_CONN_MSGTYPE_ACCEPT_REQ ? "ACCEPT_REQ" : "CONNECT_REQ",
           sysap,
           scs_connect_data_fmt(cd, rendered, sizeof(rendered)),
           memcmp(cd, scs_connect_data_vaxcluster, SCS_CONNECT_DATA_LEN) == 0
               ? " (same as ours)" : "");
    fflush(stdout);
    return 1;
}

/*
 * ===== vms-7fe: THE SDIR REFUSAL SENDER =====
 *
 * p. 2-48: "If none is found, SCS replies with a CONNECT_RSP containing the
 * 'no such SYSAP' error." p. 2-50: a second concurrent request gets "a response
 * that essentially says 'busy ... try again later'".
 *
 * ONLY THE FIRST OF THOSE TWO IS REACHABLE FROM THIS FILE, and the distinction
 * is load-bearing enough to state before anything else. The p. 2-50 busy reply
 * needs a listening CDT still in CONNECT RECEIVED when a DIFFERENT requester's
 * frame arrives; scsd_handle_frame() answers synchronously and returns the SDIR
 * to LISTEN before it returns (scs_sdir.h OVMX DESIGN CHOICE 3), so the receive
 * loop never holds that state between frames. MEASURED, not predicted:
 * tests/vmsscs/test_scsd_wire.c drives scsd_handle_frame() over every captured
 * and synthesized frame it holds, sums `sdir_busy_replies` across every case,
 * and asserts the total is 0; the exit summary prints busy-sent so a live lab
 * run stays falsifiable too. The BUSY
 * branch below is written because scs_sdir_connect_req() can return
 * SCS_SDIR_BUSY through its API, not because scsd.c has ever taken it; the only
 * caller that takes it is tests/vmsscs/test_scs_sdir.c, at module level. NO
 * TEST IN test_scsd_wire.c ASSERTS A BUSY FRAME, because there is none to
 * assert.
 *
 * WHY THIS IS NOT ONE OF THE THREE SERVICE EMITTERS, and why it is allowed to
 * call a connection-control builder (tests/vmsscs/test_scsd_send_sites.py check
 * 7 lists it beside them): the census exists to stop a hand-built frame putting
 * A CONNECTION on the wire that no CDT describes. This frame does the opposite
 * -- it DECLINES a connection. There is no CDT because p. 2-48's scan failed
 * before any SYSAP saw the request, so there is nothing for scs_accept() to
 * allocate and no Figure 2-14 transition to take; the situation is p. 2-56's
 * REJECT case ("does not involve a CDT at all, but merely the sending of a
 * message ... implemented entirely in the port driver") carrying a CONNECT_RSP
 * rather than a REJECT_REQ, because p. 2-48 says CONNECT_RSP. Routing it
 * through scs_reject() would have made the service ask for SEND_REJECT_REQ and
 * this function build a CONNECT_RSP, i.e. an emitter lying about its own frame.
 *
 * THE FRAME. The 66-byte CONNECT_RSP class, built by the SAME builder that
 * emits OVMX's positive SCS$DIRECTORY CONNECT-ECHO -- remote Con.ID echoed,
 * local Con.ID 0, [46:48] = 1. All of that is GROUNDED (spec sec 4(h)(1a), 16
 * frames / 16 dialogues; sec 4(h)(1) for the handle fill pattern; the 66-byte
 * class is observed under BOTH the 0x5b and 0x4b opcodes, 31 of 31 frames,
 * which is why `opcode` is a parameter rather than the template's baked 0x5b).
 *
 * THE STATUS WORD AT [48:50] IS NOT GROUNDED. It is an OVMX choice in both
 * placement and value -- see the WIRE STATUS block in scs_sdir.h, which also
 * records the measurement that in the configuration OVMX runs this function is
 * never called, because both CONNECT_REQs the reference VAX addresses to OVMX
 * name SYSAPs OVMX LISTENs for.
 */
static int scsd_send_sdir_refusal(int sock, int ifindex, struct peer_state *ps,
                                  const uint8_t *our_hw_mac,
                                  const uint8_t *our_src_logical,
                                  uint8_t opcode, uint32_t requester_conid,
                                  uint16_t status, const char *why)
{
    struct scs_dir_params rp;
    memset(&rp, 0, sizeof(rp));
    memcpy(rp.dst_mac, ps_port_addr(ps), 6);
    memcpy(rp.src_mac, our_hw_mac, 6);
    memcpy(rp.src_logical, our_src_logical, 6);
    memcpy(rp.peer_logical, ps_sys_addr(ps), 6);
    rp.remote_conid = requester_conid; /* echoed; local stays 0 -- CONNECT_RSP */
    rp.recv_ack = ps->vc.seq.recv_seq;
    rp.send_seq = scs_seq_advance(&ps->vc.seq);
    rp.incarnation = ps->incarnation;

    uint8_t frame[SCS_DIR_ECHO_FRAME_LEN];
    if (scs_dir_build_connect_echo(&rp, frame) != 0) {
        return 0;
    }
    /* Echo the request's opcode (0x5b directory / 0x4b sequenced-application);
     * the 66-byte CONNECT_RSP class is observed under both. */
    frame[14 + 16] = opcode;
    /* THE OVMX-CHOSEN STATUS WORD. [48:50], little endian. */
    frame[14 + 48] = (uint8_t)(status & 0xffu);
    frame[14 + 49] = (uint8_t)((status >> 8) & 0xffu);

    if (send_frame_vc(sock, ifindex, ps, ps->pb, why, frame, sizeof(frame)) <= 0) {
        sdir_refusals_unsent++;
        return 0;
    }
    if (status == SCS_SDIR_STATUS_BUSY) {
        /* NOT REACHED IN ANY RUN OF THIS DAEMON -- see the function header.
         * Counted rather than asserted away: test_scsd_wire.c reds if this
         * line ever executes, and the exit summary prints busy-sent so a live
         * run reports it too. */
        sdir_busy_replies++;
    } else {
        sdir_no_such_sysap++;
    }
    log_ts(stdout);
    printf(" SCSD-I-SDIRREFUSE, %s -> CONNECT_RSP status=0x%04X (OVMX-chosen,"
           " not grounded) opcode=0x%02x requester=0x%08X\n",
           why, (unsigned)status, opcode, requester_conid);
    fflush(stdout);
    return 1;
}

/*
 * vms-7fe: THE p. 2-48 SCAN, as the receive path runs it.
 *
 * One helper for both inbound-CONNECT_REQ paths (the 0x5b SCS$DIRECTORY connect
 * and the 0x4b VMS$VAXcluster connect), so the rule is written once. Returns
 * non-zero when the caller may proceed to ACCEPT -- i.e. the target SYSAP is
 * listening and the request was delivered to its listening CDT. Returns 0 when
 * the request was refused, having already put the refusal on the wire.
 *
 * `*sdir_out` receives the matched SDIR so the caller can return the listening
 * CDT to LISTEN with scs_sdir_connect_answered() once it has answered.
 *
 * UNDER OVMX_NO_SDIR THIS IS SKIPPED ENTIRELY by its callers -- the scan does
 * not run, no refusal is built, and the accept proceeds exactly as it did
 * before this item.
 */
/*
 * vms-7fe: IS THE p. 2-48 SCAN LIVE RIGHT NOW?
 *
 * Two conditions, and the second is not cosmetic. THE QUEUE MUST BE POPULATED.
 * scsd_cdl_ready is only set when the connection state machine is enabled, so
 * under the PRE-EXISTING kill switch OVMX_NO_CONN_FSM=1 the port never binds a
 * CDL, LISTEN never runs, and the SDIR queue is EMPTY. An empty queue read
 * literally says "no SYSAP on this node is listening", which would make the
 * scan refuse EVERY inbound connect request with p. 2-48's "no such SYSAP" --
 * turning a switch that is documented to change no byte into one that stops
 * OVMX joining at all. An empty queue means LISTEN never ran, not that nobody
 * is listening, and the honest response to "I do not know" is to behave exactly
 * as the pre-vms-7fe daemon did rather than to invent a refusal.
 *
 * Re-read on every call (never cached), like every other switch here.
 */
static int scsd_sdir_live(void)
{
    return scs_sdir_enabled() && scs_sdir_count(scs_svc_sdir(scsd_svc())) > 0;
}

static int scsd_sdir_admit(int sock, int ifindex, const uint8_t *our_hw_mac,
                           const uint8_t *our_src_logical, struct peer_state *ps,
                           const uint8_t *buf, size_t n, uint8_t opcode,
                           uint32_t requester_conid, const char *dflt_target,
                           const struct scs_sdir **sdir_out)
{
    if (sdir_out != NULL) {
        *sdir_out = NULL;
    }
    char target[SCS_CDT_SYSAP_NAME_LEN + 1];
    if (scs_sdir_target_name(buf, n, target) != 0 || target[0] == '\0') {
        /* Too short to carry the grounded [62:78] name field. Fall back to the
         * name the enclosing branch already classified the frame as, rather
         * than refusing a request whose target we simply could not read. */
        strncpy(target, dflt_target, sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
    }

    sdir_connect_scans++;
    enum scs_sdir_result r = scs_sdir_connect_req(scs_svc_sdir_mut(scsd_svc()),
                                                  target, NULL, requester_conid,
                                                  sdir_out);
    if (r == SCS_SDIR_DELIVERED) {
        return 1;
    }
    if (r == SCS_SDIR_BADARG) {
        return 0;
    }
    /* The SCS_SDIR_BUSY arm is unreachable from this daemon (scs_sdir.h DESIGN
     * CHOICE 3): a synchronous answer cannot leave a listening CDT in CONNECT
     * RECEIVED for a second requester to collide with, so every refusal scsd.c
     * has ever sent is the no-such-SYSAP one. The arm exists because the module
     * API can return BUSY, and it is written here rather than asserted away so
     * that a future asynchronous ACCEPT does not silently send status 0. */
    uint16_t status = (r == SCS_SDIR_BUSY) ? SCS_SDIR_STATUS_BUSY
                                           : SCS_SDIR_STATUS_NO_SUCH_SYSAP;
    char why[64];
    snprintf(why, sizeof(why), "CONNECT_RSP %s for '%s'",
             scs_sdir_result_name(r), target);
    (void)scsd_send_sdir_refusal(sock, ifindex, ps, our_hw_mac, our_src_logical,
                                 opcode, requester_conid, status, why);
    return 0;
}

/*
 * ===== vms-561: THE THREE EMITTERS =====
 *
 * p. 2-56's port-driver half of the five services. Each is called BY
 * scs_connect() / scs_accept() (src/vmsscs/scs_svc.c), once per transition
 * whose action names a packet, BEFORE the transition is committed -- which is
 * the order the pre-migration code used (send the frame, then step the machine)
 * and is why this refactor changes no byte on the wire.
 *
 * Each returns one of SCS_SVC_EMIT_SENT / _NOBUILDER / _REFUSED; see the emit
 * contract in scs_svc.h for what the service does with each. Every send goes
 * through send_frame_vc(), the p. 2-31 choke point, so these three are CHOKED
 * entries in the SEND SITE TABLE and no exemption is involved.
 *
 * They hold no state: everything they need is prepared by their caller and
 * handed over in a struct scsd_svc_emit_ctx, and everything they report goes
 * back the same way. That is deliberate -- it keeps the sequence-number
 * allocation, the peer bookkeeping and the run counters in the caller, where
 * they were before, rather than scattering them into callbacks.
 */
struct scsd_svc_emit_ctx {
    int sock;
    int ifindex;
    struct peer_state *ps;
    struct scs_pb *vc;                            /* the circuit the frame rides */
    const struct scs_connect_params *connect_params; /* 0x4b request/response */
    struct scs_dir_params *dir_params;            /* SCS$DIRECTORY echo + response */
    /* vms-578 (from vms-760): the requester's msgtype at abs 30, MIRRORED into
     * both SCS$DIRECTORY answer frames. An established member probes with the
     * DATA-phase 0x4b and the reference VAX answers 0x4b (ref frames
     * 162->163/164), where the byte-exact templates carry the 0x5b
     * establishing form. 0 = leave the template value. */
    uint8_t peer_msgtype;

    /* Reported back to the caller. */
    int sent;      /* the CONNECT_REQ (CONNECT) or ACCEPT_REQ (ACCEPT) went out */
    int echo_sent; /* SCS$DIRECTORY only: the op=1 CONNECT-ECHO went out */
};

/* CONNECT: the 0x4b VMS$VAXcluster CONNECT-REQUEST (spec sec 4g phase 4). */
static int scsd_svc_emit_connect_req(void *ctx, struct scs_cdt *cdt,
                                     enum scs_conn_action act,
                                     const struct scs_svc_args *args,
                                     const char **what)
{
    struct scsd_svc_emit_ctx *e = (struct scsd_svc_emit_ctx *)ctx;
    (void)args;
    if (act != SCS_CONN_ACT_SEND_CONNECT_REQ) {
        return scsd_svc_no_builder(cdt, act);
    }
    uint8_t cframe[SCS_CONNECT_FRAME_LEN];
    if (scs_connect_build_request(e->connect_params, cframe) != 0) {
        return SCS_SVC_EMIT_REFUSED;
    }
    /* vms-760, carried through the vms-561 service path by vms-578: match the
     * 2->3 reference (vax3-2to3-established-join) byte-for-byte -- a JOINER-
     * INITIATED VMS$VAXcluster connect is msgtype 0x5b (like its dir + MSCP
     * connects; the VC is not up yet), and the 4 descriptor bytes at abs
     * 112/114/116/118 are ZERO. The vms-d94 path emitted 0x4b + 01s, grounded
     * on a LATER-PHASE frame, which the established member does not accept.
     *
     * WHY IT IS HERE AND NOT AT THE CALL SITE: worktree-760 applied its stores
     * inline next to its own send_frame_to(). That send site no longer exists --
     * the CONNECT service owns the emit -- so the delta moves into the emitter,
     * the single place the CONNECT-REQUEST frame is now built. Kill switch:
     * OVMX_CONNREQ_LEGACY_MSGTYPE=1 restores the 0x4b for bisecting.
     *
     * GUARDRAIL 23 -- THE SWITCH WAS RUN, AND IT MOVES THE BYTE. lab-2 pod
     * vaxlab-4, 2026-08-05, arms B2 (default) and B5 (switch set), read off the
     * captures rather than the log: selecting OVMX-sourced 110-byte SCA frames
     * whose [46:48] is message type 0 and whose [62:78] is 'VMS$VAXcluster',
     * SCA[16] is 0x5b,0x5b with the switch off and 0x4b,0x4b with it on.
     * BOTH ARMS JOINED (CLUSTER_NODES=3, XITDONE=1), so on THIS lab the byte is
     * not what decides a first join -- see spec sec 4(O.1).
     *
     * ONLY THE MSGTYPE SURVIVES, AND THAT IS A MEASURED DELETION. worktree-760
     * also zeroed abs 112/114/116/118 here. Those four bytes sit INSIDE the
     * p. 2-25 connect-data region (abs 108..123) that vms-fdd already stamps,
     * and vms-fdd's value carries 0 at every one of them -- it is the joiner
     * form, from 148/148 VAX-sourced frames, where worktree-760 read one
     * reference. So the four stores were a NO-OP against the shipped build and
     * a LIVE one against OVMX_NO_CONNECT_DATA=1: they re-zeroed exactly the
     * four offsets at which the template and the stamp differ, which silently
     * disarmed that kill switch. test_connect_data_rides_the_daemon measured
     * it. Deleted; vms-fdd's stamp already puts worktree-760's bytes on the
     * wire, from the better census. */
    if (getenv("OVMX_CONNREQ_LEGACY_MSGTYPE") == NULL) {
        cframe[30] = SCS_DIR_OPCODE;   /* abs 30 = SCA [16], msgtype 0x5b */
    }
    *what = "VMS$VAXcluster CONNECT-REQUEST";
    if (send_frame_vc(e->sock, e->ifindex, e->ps, e->vc, *what,
                      cframe, sizeof(cframe)) <= 0) {
        return SCS_SVC_EMIT_REFUSED;
    }
    e->sent = 1;
    return SCS_SVC_EMIT_SENT;
}

/*
 * ACCEPT, SCS$DIRECTORY. Two frames for the two transitions of Figure 2-14's
 * NODE_2 column, and spec sec 4(h)(1) grounds the pairing:
 *   CONNECT_RSP = op=1 CONNECT-ECHO   (SCA23, "VAX2 echoes VAX1's handle, its
 *                                      own not yet assigned")
 *   ACCEPT_REQ  = op=2 CONNECT-RESPONSE (SCA25, "VAX2 supplies its own handle
 *                                      -- pair now bound")
 * Each is a sequenced message, so each advances OVMX's send_seq (spec sec
 * 4h(4)) at the moment it is built -- exactly as the pre-migration code did,
 * and in the same order.
 */
static int scsd_svc_emit_dir_accept(void *ctx, struct scs_cdt *cdt,
                                    enum scs_conn_action act,
                                    const struct scs_svc_args *args,
                                    const char **what)
{
    struct scsd_svc_emit_ctx *e = (struct scsd_svc_emit_ctx *)ctx;
    (void)args;
    struct scs_dir_params *dp = e->dir_params;

    if (act == SCS_CONN_ACT_SEND_CONNECT_RSP) {
        dp->recv_ack = e->ps->vc.seq.recv_seq;
        dp->send_seq = scs_seq_advance(&e->ps->vc.seq);
        uint8_t eframe[SCS_DIR_ECHO_FRAME_LEN];
        if (scs_dir_build_connect_echo(dp, eframe) != 0) {
            return SCS_SVC_EMIT_REFUSED;
        }
        /* vms-578 (from vms-760): answer in the CONNECTION's phase, not the
         * template's. See scsd_svc_emit_ctx.peer_msgtype. */
        if (e->peer_msgtype != 0) {
            eframe[30] = e->peer_msgtype;
        }
        *what = "SCS$DIRECTORY op=1 CONNECT-ECHO";
        /* The pre-migration code took this transition on a successful BUILD,
         * not on a successful send, so a refused send here must not abandon the
         * ACCEPT that follows. Report NOBUILDER, which commits the transition
         * and counts the frame as not sent. */
        if (send_frame_vc(e->sock, e->ifindex, e->ps, e->ps->pb, *what,
                          eframe, sizeof(eframe)) <= 0) {
            return SCS_SVC_EMIT_NOBUILDER;
        }
        e->echo_sent = 1;
        return SCS_SVC_EMIT_SENT;
    }

    if (act == SCS_CONN_ACT_SEND_ACCEPT_REQ) {
        dp->recv_ack = e->ps->vc.seq.recv_seq;
        dp->send_seq = scs_seq_advance(&e->ps->vc.seq);
        uint8_t rframe[SCS_DIR_RESP_FRAME_LEN];
        if (scs_dir_build_connect_response(dp, rframe) != 0) {
            return SCS_SVC_EMIT_REFUSED;
        }
        if (e->peer_msgtype != 0) {
            rframe[30] = e->peer_msgtype;   /* vms-578 (from vms-760): mirror */
        }
        *what = "SCS$DIRECTORY op=2 CONNECT-RESPONSE";
        if (send_frame_vc(e->sock, e->ifindex, e->ps, e->ps->pb, *what,
                          rframe, sizeof(rframe)) <= 0) {
            return SCS_SVC_EMIT_REFUSED;
        }
        e->sent = 1;
        return SCS_SVC_EMIT_SENT;
    }
    return scsd_svc_no_builder(cdt, act);
}

/*
 * ACCEPT, member-opened VMS$VAXcluster. ONE frame for TWO transitions, and the
 * asymmetry is the honest part: the machine requires a CONNECT_RSP first and
 * OVMX BUILDS NONE. That is an OVMX gap, not a wire gap -- the real VAX does
 * send that frame (the 66-byte message-type-1 class, 16 of 16 dialogues, spec
 * sec 4(h)(1a)) -- so the CONNECT_RSP arm reports NOBUILDER and is logged, and
 * only the ACCEPT_REQ (the 110-byte 0x4b CONNECT-RESPONSE carrying both
 * Con.IDs, golden frame 50, message type 2) goes on the wire.
 */
static int scsd_svc_emit_member_accept(void *ctx, struct scs_cdt *cdt,
                                       enum scs_conn_action act,
                                       const struct scs_svc_args *args,
                                       const char **what)
{
    struct scsd_svc_emit_ctx *e = (struct scsd_svc_emit_ctx *)ctx;
    (void)args;
    if (act != SCS_CONN_ACT_SEND_ACCEPT_REQ) {
        return scsd_svc_no_builder(cdt, act);
    }
    uint8_t rframe[SCS_CONNECT_FRAME_LEN];
    if (scs_connect_build_response(e->connect_params, rframe) != 0) {
        return SCS_SVC_EMIT_REFUSED;
    }
    *what = "VMS$VAXcluster 0x4b CONNECT-RESPONSE";
    if (send_frame_vc(e->sock, e->ifindex, e->ps, e->ps->pb, *what,
                      rframe, sizeof(rframe)) <= 0) {
        return SCS_SVC_EMIT_REFUSED;
    }
    e->sent = 1;
    return SCS_SVC_EMIT_SENT;
}

/*
 * ================= vms-591: THE DISCONNECT DIALOGUE (Figure 2-16) ==========
 *
 * The port-driver half of DISCONNECT: the emitter for the two actions the
 * connection state machine names on the teardown arrows, SEND_DISCONNECT_REQ
 * and SEND_DISCONNECT_RSP. Both frames are built by src/vmsscs/scs_disc.c from
 * byte-exact captured templates; read scs_disc.h for the grounding, including
 * why the DISCONNECT_RSP -- which the spec said was not on our wire -- is.
 *
 * WHICH CONNECTION. The frame is addressed from the CDT: local Con.ID is the
 * CDT's own handle, remote Con.ID is the peer's handle recorded on it. That
 * matters because a peer has up to three connections to OVMX at once and the
 * daemon must be able to tear down exactly the one being disconnected.
 *
 * EVERY SEND IS SEQUENCED. Both frames are SCS sequenced messages, so each
 * takes its own send_seq from the peer's live VC (scs_seq_advance) at the
 * moment it is built, exactly as the SCS$DIRECTORY accept pair does. A
 * DISCONNECT frame carrying a stale sequence number is the failure mode
 * vms-c6d already had to fix once on the 0x4b connect.
 *
 * CHOKED, per the SEND SITE TABLE: both sends go through send_frame_vc(), so
 * p. 2-31 applies -- OVMX will not disconnect over a circuit that is not OPEN.
 * That is the right answer and not a limitation: if the circuit is gone, the
 * connections it carried are already broken by scs_vc_break(), which drives
 * every one of them to CLOSED through SCS_CONN_EV_VC_LOST. There is nothing
 * left to disconnect and no circuit to disconnect it over.
 */
struct scsd_disc_emit_ctx {
    int sock;
    int ifindex;
    struct peer_state *ps;
    const uint8_t *our_hw_mac;
    const uint8_t *our_src_logical;
    uint16_t reason;   /* p. 2-26's optional reason code (vms-6b3) */
    int matching;      /* the [60:62] flag: is this REQ answering the peer's? */
    int req_sent;
    int rsp_sent;
};

static int scsd_svc_emit_disconnect(void *ctx, struct scs_cdt *cdt,
                                    enum scs_conn_action act,
                                    const struct scs_svc_args *args,
                                    const char **what)
{
    struct scsd_disc_emit_ctx *e = (struct scsd_disc_emit_ctx *)ctx;
    struct scs_disc_params dp;
    uint8_t frame[SCS_DISC_REQ_FRAME_LEN];
    size_t len;
    (void)args;

    if (act != SCS_CONN_ACT_SEND_DISCONNECT_REQ &&
        act != SCS_CONN_ACT_SEND_DISCONNECT_RSP) {
        return scsd_svc_no_builder(cdt, act);
    }
    /* No CDT means no Con.ID pair, and a DISCONNECT frame without one names no
     * connection. Descriptorless mode (scs_svc.h) therefore cannot disconnect;
     * say so rather than emitting a frame addressed to connection zero. */
    if (cdt == NULL) {
        return scsd_svc_no_builder(cdt, act);
    }

    memset(&dp, 0, sizeof(dp));
    memcpy(dp.dst_mac, ps_port_addr(e->ps), 6);
    memcpy(dp.src_mac, e->our_hw_mac, 6);
    memcpy(dp.src_logical, e->our_src_logical, 6);
    memcpy(dp.peer_logical, ps_sys_addr(e->ps), 6);
    dp.remote_conid = cdt->remote_conid;
    dp.local_conid = cdt->local_conid;
    dp.incarnation = e->ps->incarnation;
    dp.recv_ack = e->ps->vc.seq.recv_seq;
    dp.send_seq = scs_seq_advance(&e->ps->vc.seq);
    dp.reason = e->reason;
    dp.matching = e->matching;

    if (act == SCS_CONN_ACT_SEND_DISCONNECT_REQ) {
        if (scs_disc_build_request(&dp, frame) != 0) {
            /* The ONLY way this fails with a non-NULL params is the kill
             * switch. NOBUILDER is then literally true -- with
             * OVMX_NO_CLEAN_SHUTDOWN=1 OVMX has no DISCONNECT_REQ builder,
             * which is the state it was in before this item. */
            return scsd_svc_no_builder(cdt, act);
        }
        len = SCS_DISC_REQ_FRAME_LEN;
        *what = "DISCONNECT_REQ";
    } else {
        if (scs_disc_build_response(&dp, frame) != 0) {
            return scsd_svc_no_builder(cdt, act);
        }
        len = SCS_DISC_RSP_FRAME_LEN;
        *what = "DISCONNECT_RSP";
    }

    if (send_frame_vc(e->sock, e->ifindex, e->ps, e->ps->pb, *what,
                      frame, len) <= 0) {
        return SCS_SVC_EMIT_REFUSED;
    }
    if (act == SCS_CONN_ACT_SEND_DISCONNECT_REQ) {
        e->req_sent++;
        disc_req_sent++;
    } else {
        e->rsp_sent++;
        disc_rsp_sent++;
    }
    return SCS_SVC_EMIT_SENT;
}

/*
 * scsd_disc_args - the service argument set for a teardown. Only the emitter,
 * the reason and (through the ctx) the matching flag matter here: DISCONNECT
 * allocates nothing, so none of the CONNECT/ACCEPT arguments apply.
 */
static struct scs_svc_args scsd_disc_args(struct scsd_disc_emit_ctx *e, uint16_t reason)
{
    struct scs_svc_args a;
    memset(&a, 0, sizeof(a));
    e->reason = reason;
    a.reason = reason;
    a.emit = scsd_svc_emit_disconnect;
    a.emit_ctx = e;
    return a;
}

/*
 * scsd_disconnect_dialogue - drive Figure 2-16 for ONE received DISCONNECT
 * message on ONE connection.
 *
 * p. 2-26, the rule this implements:
 *
 *   "the symmetry that SCA builds into the concept of a connection requires
 *    that the other SYSAP respond by also invoking the DISCONNECT service"
 *
 * so a received DISCONNECT_REQ is answered in TWO steps, not one:
 *
 *   1. DELIVER it. From OPEN that is Figure 2-16's OPEN --RCV_DISCONNECT_REQ-->
 *      DISC RECEIVED with action send DISCONNECT_RSP and the SYSAP-disconnected
 *      notification. From DISC SENT it is the p. 2-27 SIMULTANEOUS case,
 *      straight to DISC MATCH with the same answer. From DISC ACK it is the
 *      last arrow of the figure, to CLOSED.
 *
 *   2. If, and only if, that left the connection in DISC RECEIVED, the owning
 *      SYSAP now INVOKES ITS OWN DISCONNECT -- DISC RECEIVED --SVC_DISCONNECT-->
 *      DISC MATCH, sending the MATCHING DISCONNECT_REQ ([60:62] = 1). In the
 *      simultaneous case step 2 is skipped because our own DISCONNECT_REQ has
 *      already gone out; that is what "transitioning through only three states"
 *      (p. 2-27) means, and the state test is how this code knows which case it
 *      is in rather than guessing from timing.
 *
 * WHICH SYSAP INVOKES IT. OVMX has no asynchronous SYSAP to hand the
 * notification to and no disconnect callback on the CDT, so scsd.c invokes the
 * symmetric DISCONNECT itself, synchronously, in the receive path. That is the
 * SAME labeled difference scs_svc.h already records for ACCEPT ("an OVMX SYSAP
 * cannot currently take time to decide"), stated again here rather than left to
 * be inferred: the p. 2-26 SYSAP notification is LOGGED (the CONNFSM line's
 * notify=SYSAP-disconnected bit) and is not DELIVERED to any handler, because
 * there is no handler to deliver it to. The one handler a CDT does carry is the
 * VC-LOSS handler, and calling that for a disconnect would report a circuit
 * loss that did not happen -- scs_svc.h refuses it for exactly that reason.
 *
 * Returns 1 if the connection reached CLOSED and its CDT was released (after
 * which `cdt` must not be touched again).
 */
static int scsd_disconnect_dialogue(int sock, int ifindex, struct peer_state *ps,
                                    struct scs_cdt *cdt, enum scs_conn_event ev,
                                    const uint8_t our_hw_mac[6],
                                    const uint8_t our_src_logical[6])
{
    struct scsd_disc_emit_ctx e;
    struct scs_svc_port *port = scsd_svc();
    enum scs_conn_state before;
    int closed = 0;

    if (port == NULL || cdt == NULL || ps == NULL) {
        return 0;
    }
    memset(&e, 0, sizeof(e));
    e.sock = sock;
    e.ifindex = ifindex;
    e.ps = ps;
    e.our_hw_mac = our_hw_mac;
    e.our_src_logical = our_src_logical;

    before = scs_conn_state_of(cdt);
    if (ev == SCS_CONN_EV_RCV_DISCONNECT_REQ) {
        disc_req_recv++;
    } else {
        disc_rsp_recv++;
    }

    /* Step 1. Answering a peer's DISCONNECT_REQ is an answer, not an
     * initiation, so the DISCONNECT_RSP carries no matching flag at all (the
     * 58-byte class has no such field) and the reason code is the peer's
     * business, not ours. */
    {
        struct scs_svc_args a = scsd_disc_args(&e, SCS_REASON_NONE);
        struct scsd_svc_mark m = scsd_svc_mark();
        (void)scs_svc_deliver(port, cdt, ev, &a, &closed);
        scsd_svc_settle(m);
    }
    if (closed) {
        disc_closed++;
        log_ts(stdout);
        printf(" SCSD-I-DISCCLOSED, connection closed and CDT released after"
               " the %s teardown\n",
               before == SCS_CONN_DISC_MATCH ? "matching" : "peer-initiated");
        fflush(stdout);
        return 1;
    }

    /* p. 2-27: BOTH sides had already sent a DISCONNECT_REQ. We answered it
     * above and are now in DISC MATCH with nothing further to invoke. */
    if (before == SCS_CONN_DISC_SENT && ev == SCS_CONN_EV_RCV_DISCONNECT_REQ &&
        scs_conn_state_of(cdt) == SCS_CONN_DISC_MATCH) {
        disc_simultaneous++;
        log_ts(stdout);
        printf(" SCSD-I-DISCSIMUL, conid=0x%08X: simultaneous disconnect"
               " (p. 2-27) -- both ends had sent DISCONNECT_REQ; answered and"
               " went straight to DISC MATCH\n", (unsigned)cdt->local_conid);
        fflush(stdout);
        return 0;
    }

    /* Step 2. THE SYMMETRIC HALF -- the one OVMX never performed. */
    if (scs_conn_state_of(cdt) == SCS_CONN_DISC_RECEIVED) {
        struct scs_svc_args a = scsd_disc_args(&e, SCS_REASON_PEER_DISCONNECT);
        struct scsd_svc_mark m = scsd_svc_mark();
        e.matching = 1;
        (void)scs_disconnect(port, cdt, &a);
        scsd_svc_settle(m);
        log_ts(stdout);
        printf(" SCSD-I-DISCMATCH, conid=0x%08X: SYSAP invoked its own"
               " DISCONNECT (p. 2-26) -- matching DISCONNECT_REQ %s\n",
               (unsigned)cdt->local_conid,
               e.req_sent ? "sent" : "NOT sent (no builder or refused)");
        fflush(stdout);
    }
    return 0;
}

/*
 * send_joiner_connect_request - vms-d94: send OVMX's ACTIVE-JOINER
 * VMS$VAXcluster CONNECT-REQUEST to the member (remote=0, offering
 * OVMX_JOINER_CONID). A real joiner opens its OWN connection to the member's
 * connection manager and drives the add-member on it (clean-ref idx52). Timing
 * matters: the member's CM times out ~1.4s after START and re-issues START, so
 * this must fire PROMPTLY (right after the post-START directory phase begins),
 * not on a lazy HELLO poll. The request is one sequenced message: the first
 * send allocates a send_seq; retransmits REUSE it (a retransmit is not a new
 * message). Returns 1 if a frame was sent.
 *
 * vms-398 -- WHICH VIRTUAL CIRCUIT (VAXcluster Principles p. 2-47). CONNECT
 * takes either a named circuit or none: "CONNECT ... uses CONFIG_SYS to obtain
 * the address of the first Path Block queued to the System Block for the
 * specified node ... [and] examines each Path Block in turn until it finds one
 * whose virtual circuit is OPEN".
 *   named_vc != NULL - the caller named the circuit; it is used as given.
 *   named_vc == NULL - the caller named only the NODE. The circuit is selected
 *                      from the configuration database by the peer's 48-bit SCS
 *                      System Address, via scs_config_select_vc() (CONFIG_SYS +
 *                      the OPEN scan). This is what BOTH of the daemon's own
 *                      call sites pass.
 * If no OPEN circuit exists for the node, NOTHING IS SENT and the refusal is
 * logged: there is no honest frame to build without a circuit (a fabricated one
 * would carry a zero peer-logical), and a silent success is exactly the failure
 * mode CLAUDE.md rule 9 / INV-6 forbids.
 *
 * The circuit's characteristics -- the remote port address the frame is
 * addressed to and the System Block whose System Address goes in the SCA
 * peer-logical field -- are read back through CONFIG_PATH (p. 2-47), not by
 * reaching into the Path Block, so the daemon sees the same view of a path that
 * any other CONFIG_PATH caller would.
 */
static int send_joiner_connect_request(int sock, int ifindex, struct scs_config *cfg,
                                       struct peer_state *ps, struct scs_pb *named_vc,
                                       const uint8_t our_hw_mac[6],
                                       const uint8_t our_src_logical[6])
{
    /* vms-760 server-first: when OVMX_PURE_SERVER is set, OVMX does NOT open its
     * own VMS$VAXcluster VC. The established member DRIVES admission -- it opens
     * the MSCP$DISK connect AND the VC itself (af2-firsttimer-established). OVMX's
     * pre-emptive active-joiner VC (vms-d94) short-circuits that to status NEW and
     * the member never opens the MSCP$DISK connect that leads to MEMBER. As a pure
     * server OVMX answers the member's dir/lookups (serving MSCP$DISK + VMS$VAXcluster
     * affirmative) and accepts the member-opened connects.
     *
     * vms-578: this test comes FIRST, before the p. 2-47 circuit selection below.
     * Refusing to open a connection at all is not the same as being refused a
     * send on a closed circuit, and only the former should be silent. */
    if (getenv("OVMX_PURE_SERVER") != NULL) {
        return 0;
    }

    struct scs_pb *vc = named_vc;
    if (vc == NULL) {
        vc = scs_config_select_vc(cfg, ps_sys_addr(ps));
    }
    struct scs_config_path_info path;
    /* vms-abc: the refusal is the CHOKE POINT's, not a bespoke one. It used to
     * be an inline fprintf here, which is where the ~1 Hz SCSD-E-NOVC flood
     * after a break came from: the daemon re-enters this function once per
     * directed HELLO and every entry printed. Routing it through
     * scsd_refuse_without_open_vc() gives it the same per-circuit latch and the
     * same vc_sends_refused accounting as every other refused send; the p. 2-47
     * specifics that used to be in the format string are carried in `what`.
     * NOTE the condition is now STRICTER: the old test was "CONFIG_PATH
     * describes it", which a CLOSED PB satisfies -- only scs_config_select_vc()
     * checked OPEN, and only on the named_vc == NULL path. A caller that NAMED
     * a CLOSED circuit got a frame. */
    if (scsd_refuse_without_open_vc(
            ps, vc,
            "VMS$VAXcluster CONNECT-REQUEST (p. 2-47: CONNECT needs an open"
            " circuit; none was named and CONFIG_SYS found none for this node)",
            &path)) {
        return 0;
    }

    struct scs_connect_params cp;
    memset(&cp, 0, sizeof(cp));
    memcpy(cp.dst_mac, path.remote_port_addr, 6);
    memcpy(cp.src_mac, our_hw_mac, 6);
    memcpy(cp.src_logical, our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
    memcpy(cp.peer_logical, path.sb != NULL ? path.sb->system_id : zero_addr, 6);
    cp.local_conid = PS_JOINER_CONID(ps);
    cp.remote_conid = 0; /* CONNECT-REQUEST: the member's Con.ID is not yet known */
    cp.recv_ack = ps->vc.seq.recv_seq; /* always ack the member's latest send_seq */
    /* vms-abc: FIRST send or RETRANSMIT? The sequence number is allocated once
     * and reused, so "joiner_req_seq is still 0" IS the discriminator, and it
     * has to be read BEFORE the allocation below. */
    int first_send = (ps->joiner_req_seq == 0);
    if (first_send) {
        ps->joiner_req_seq = scs_seq_advance(&ps->vc.seq); /* allocate once */
    }
    cp.send_seq = ps->joiner_req_seq; /* retransmits reuse the same seq */
    cp.incarnation = ps->incarnation;

    /* vms-561: THE CONNECT SERVICE (pp. 2-22..2-23, 2-47, 2-56). The frame is
     * still built and sent by this file -- scsd_svc_emit_connect_req() below is
     * handed the params prepared above -- but the CDT, the p. 2-28/2-29 SYSAP
     * entry points and the Figure 2-14 transition are the service's, not
     * scsd.c's. The circuit is passed as args.vc because THIS function has
     * already run p. 2-47's selection (above) in order to address the frame;
     * scs_connect() would otherwise repeat it. */
    struct scsd_svc_emit_ctx ec;
    memset(&ec, 0, sizeof(ec));
    ec.sock = sock;
    ec.ifindex = ifindex;
    ec.ps = ps;
    ec.vc = vc;
    ec.connect_params = &cp;

    struct scs_svc_args a;
    memset(&a, 0, sizeof(a));
    a.local_sysap = "VMS$VAXcluster";
    a.remote_sysap = "VMS$VAXcluster";
    a.target_node = ps_sys_addr(ps);
    a.cfg = cfg;
    a.vc = vc;
    a.vc_loss = scsd_sysap_vc_loss;
    /* vms-7c0: p. 2-29 -- the message input routine is an argument to CONNECT
     * and ACCEPT, and this is the first OVMX build that supplies one. No
     * datagram input routine is supplied; see the note under
     * scsd_sysap_msg_input() for the measurement that says why. */
    a.msg_input = scsd_sysap_msg_input;
    a.sysap_ctx = ps;
    a.conid = PS_JOINER_CONID(ps);
    a.emit = scsd_svc_emit_connect_req;
    a.emit_ctx = &ec;

    struct scsd_svc_mark mark = scsd_svc_mark();
    struct scs_cdt *cdt = NULL;
    enum scs_svc_status st = scs_connect(scsd_svc(), &a, &cdt);
    scsd_svc_settle(mark);
    if (st == SCS_SVC_NOCDT) {
        scsd_svc_slot_refused(PS_JOINER_CONID(ps), "VMS$VAXcluster");
    }
    if (ec.sent) {
        if (cdt != NULL) {
            ps->cdt_joiner = cdt;
            /* vms-fdd: p. 2-28 puts the SYSAP's connect data in the CDT, and
             * this is the CONNECT service's copy -- the same 16 bytes
             * scs_connect_build_request() just stamped at [94:110]. Carried,
             * not interpreted; the owning SYSAP reads it back off the CDT. */
            scsd_cdt_record_connect_data(cdt);
        }
        ps->joiner_connect_sent = 1;
        clock_gettime(CLOCK_MONOTONIC, &ps->last_joiner_req);
        /* vms-abc -- THE p. 2-31 DELIVERY GUARANTEE'S ONLY LIVE FEED.
         *
         * This function is the ONLY caller of scs_vc_record_sent() in OVMX
         * (`grep -rn scs_vc_record_sent src/vmsscs/` -> its definition in
         * scs_vc.c, this comment, and the one call below), so the joiner
         * CONNECT-REQUEST is the only outstanding sequenced message the
         * delivery detector can ever see.
         *
         * It used to call scs_vc_record_sent() on EVERY send. That contradicts
         * this function's own contract two paragraphs up -- "retransmits REUSE
         * it (a retransmit is not a new message)" -- because record_sent()
         * RESETS vc.retransmit_count to 0. The counter therefore never left 0
         * in a real run no matter how many times the member ignored us, and
         * scs_vc_delivery_failed() could not become true in production. Fixing
         * that is what gives the delivery break a live path.
         *
         * NOT A WIRE CHANGE: both calls only touch OVMX's local VC bookkeeping.
         * The frame is byte-identical either way (same builder, same reused
         * cp.send_seq), and the retry CAP is unchanged. What changes is that
         * exhausting the retries is now DETECTABLE. */
        if (first_send) {
            scs_vc_record_sent(&ps->vc, cp.send_seq, monotonic_ms());
        } else {
            scs_vc_mark_retransmitted(&ps->vc, monotonic_ms());
        }
        return 1;
    }
    return 0;
}

/*
 * ===== vms-fb1: THE RECEIVE-DISPATCH SEAM =====
 *
 * Everything the per-frame handler touches, gathered into one object so the
 * handler can be a FUNCTION instead of nine hundred lines inline in main().
 * main()'s loop is now recv() + scsd_handle_frame(), and the exit report is
 * scsd_exit_summary().
 *
 * WHY THIS SHAPE. SCSD_UNIT_TEST renames main() away, so until now every branch
 * below the recv() was compiled but reachable from no test. That is a MEASURED
 * gap, not a hypothetical one: the vms-dd5 adversary pass applied three mutants
 * inside this loop (retargeting the SCS$DIRECTORY and VMS$VAXcluster conn_step
 * calls, and deleting the exit-summary scs_conn_report_stuck call) and all three
 * SURVIVED the full suite. vms-4071 hit the same wall from the other side. With
 * the dispatch behind a function, a test supplies its own struct scsd_rx and a
 * captured frame and drives the SAME production code the daemon runs.
 *
 * NOTHING ABOUT THE WIRE CHANGES HERE: the bodies below are the loop bodies,
 * moved, with `continue` becoming `return` (the loop body contained no inner
 * loop, so every continue was a loop-level one) and loop-local state reached
 * through `rx->`.
 */
struct scsd_rx {
    int sock;
    int ifindex;
    const uint8_t *our_hw_mac;
    const uint8_t *our_src_logical;
    const uint8_t *lab_nonce;
    struct scs_hello_params *hello_params;
    struct scs_config *cfg;
    struct scs_pdt *pdt;
    struct peer_state *peers;
    struct scsd_vc_ctx *vc_ctx;
    const char *ifname;
    int respond;
    int do_connect;
    int emit_hello;
    /* vms-578: worktree-760's OVMX_JOIN_SEQ flag. It was a main() local there;
     * the dispatch is a function here, so it has to travel with the context. */
    int join_seq_enabled;

    /* Run counters, reported by scsd_exit_summary(). */
    long counts[5];
    long total_frames;
    long hello_sent;
    long directed_sent;
    long connect_req_sent;
    long connect_resp_sent;
    long start_sent;
    long start_ack_sent;
    long credit_sent;        /* vms-691: total 0x48 credit-returns sent */
    long retransmit_sent;    /* vms-691: connect-request retransmits sent */
    long dir_conn_resp_sent; /* vms-246: SCS$DIRECTORY CONNECT-RESPONSEs sent */
    long dir_lookup_sent;    /* vms-246: SCS$DIR_LOOKUP responses sent */
    long poll_connect_sent;  /* vms-66f: SCS$DIR_LOOKUP -> SCS$DIRECTORY CONNECT_REQs */
    long poll_inquiry_sent;  /* vms-66f: p. 2-50 inquiries put on the wire */
    long poll_answers;       /* vms-66f: lookup RESPONSEs fed back to the poller */
    long poll_found;         /* vms-66f: affirmative discoveries notified */
    long poll_disconnect_sent; /* vms-66f r4: p. 2-50 cycle-closing DISCONNECT_REQs */
    long cm_config_frames;   /* vms-224: op 0x14/0x01/0x02 CM config frames sent */
    long cm_response_sent;   /* vms-224: 0x81 responses to member 0x03/0x05 txns */
    long padded_sent;        /* vms-9f3: padded directed HELLOs sent (sec 4k) */
    /* vms-578: hoisted out of main()'s locals on worktree-760 so the
     * SCSD_UNIT_TEST seam (which renames main() away) can reach them. */
    long cm_abort_seen;      /* vms-2f3/vms-c21: cat-0x01 op-0x04 role-0x50 CM
                              * barrier messages seen -- NOT "aborts": this
                              * message occurs on successful joins too and does
                              * not signal refusal (spec 4(O.33)). */
    long credit_retx_seen;   /* vms-2f3 sec 4M.14: RETRANSMITTED (0x7b) op6/op8 credit
                              * handshakes answered. 0 in every join, 2 in every
                              * refused rejoin measured. */
    long mscp_srv_accepts;   /* vms-760: op=4 MSCP$DISK connect-ACCEPTs we sent */
    long psc_ungated;        /* vms-2f3 step 4: disk-discovery runs started UNGATED */

    /* vms-9f3: reusable padded-HELLO send buffer (up to 1514B on the wire),
     * held here to keep it off the handler's stack frame. */
    uint8_t pframe[SCS_HELLO_PADDED_MAX_FRAME];
};

/*
 * scsd_hello_beacon_emit - build and transmit ONE periodic multicast HELLO, the
 * beacon a node announces itself with. Returns what send_frame_channel()
 * returned, or -1 if the frame could not be built or could not be sent.
 *
 * WHY IT IS A FUNCTION. It was six lines inside main()'s timer loop, and the
 * SCSD_UNIT_TEST seam RENAMES main() AWAY -- so this send, alone among the
 * daemon's senders, was reachable from no test at all. That is also how it came
 * to call sendto() on the socket directly and sit in neither half of the SEND
 * SITE TABLE: nothing could look at it. Hoisted here it is an ordinary static
 * helper, driven by tests/vmsscs/test_scsd_wire.c
 * (test_the_hello_beacon_transmits_through_the_channel_exemption) against the
 * capture buffer, exactly as every other sender in this file is.
 *
 * THE CADENCE STAYS IN THE LOOP. This function does not consult the clock and
 * has no idea how often it is called; main() still owns `last_hello` and the
 * --hello-interval comparison. Only the build-and-send moved, so the wire sees
 * the same frames at the same times.
 *
 * It is a HELLO, so it takes the send_frame_channel() exemption: it is
 * addressed to the cluster MULTICAST group rather than to any port address, and
 * it is transmitted when no peer, no System Block, no Path Block and therefore
 * no virtual circuit is known at all -- it is how peers are discovered in the
 * first place. See the EXEMPT half of the SEND SITE TABLE.
 */
static ssize_t scsd_hello_beacon_emit(struct scsd_rx *rx, struct scs_hello_params *hp,
                                      int sock, int ifindex)
{
    uint8_t frame[SCS_HELLO_FRAME_LEN];
    hp->timer_tick = hello_timer_tick100(); /* vms-9f3: live 100ns tick */
    if (scs_hello_build_frame(hp, frame) != 0) {
        return -1;
    }
    ssize_t sent = send_frame_channel(sock, ifindex, hp->dst_mac, frame, sizeof(frame));
    if (sent < 0) {
        fprintf(stderr, "SCSD-E-SENDFAIL, HELLO beacon transmit failed: %s\n",
                strerror(errno));
        return -1;
    }
    rx->hello_sent++;
    log_ts(stdout);
    printf(" SCSD-I-HELLOSENT, node='%s' seq=%ld bytes=%zd\n",
           hp->node_name, rx->hello_sent, sent);
    fflush(stdout);
    return sent;
}

/*
 * scsd_joiner_retransmit_pending - vms-abc: EXACTLY the condition under which
 * the daemon retransmits its own outstanding joiner CONNECT-REQUEST, extracted
 * from the branch that used to spell it inline (the directed-HELLO handler,
 * SCSD-I-CONNREQ "retransmit").
 *
 * WHY IT IS A FUNCTION AND NOT AN `if`. This predicate is the whole
 * reachability argument for the p. 2-31 DELIVERY break: retransmits are what
 * advance vc.retransmit_count, and nothing else in OVMX advances it. A test
 * that drives the retransmit therefore has to be able to assert the SAME
 * predicate the dispatch branches on -- otherwise the test's claim "the daemon
 * would have taken this path" is an unchecked assertion in a comment, which is
 * how the previous revision of this file ended up testing `connect_sent`, a
 * field production never writes. With the predicate shared, narrowing the guard
 * reds the test.
 *
 * `pb != NULL` is not in the original inline guard because the enclosing branch
 * already established it; it is spelled out here so the predicate is safe to
 * call on any peer slot.
 *
 * THE OPEN-CIRCUIT TERM IS NOT COSMETIC -- it fixes a live symptom. Every other
 * term stays TRUE FOREVER after a delivery break: the break leaves the Path
 * Block in place (scs_vc.h note 4 -- breaking a circuit does not deallocate
 * it), start_acked and joiner_connect_sent are latched, and joiner_connected is
 * CLEARED by scsd_sysap_vc_loss(), which this predicate reads NEGATED. So the
 * daemon kept re-entering this branch once per directed HELLO, roughly 1 Hz,
 * for the rest of the run -- each time calling send_joiner_connect_request(),
 * which found no OPEN circuit and logged SCSD-E-NOVC. Honest (it never faked a
 * send) but endless, and the retransmit it was attempting is exactly what
 * p. 2-31 forbids on a broken circuit. Asking the Path Block up front stops the
 * attempt at its source rather than rate-limiting its complaint. The circuit is
 * read through CONFIG_PATH (p. 2-47) for the same reason send_frame_vc() does.
 *
 * IT IS NOT THE ENFORCEMENT, though, and must not be read as such: this
 * predicate stops one ATTEMPT early. Whether a frame may leave is decided in
 * send_frame_vc() for every sender in the file. If this term were deleted the
 * frame would still not go out -- only the log would get noisy again. That is
 * the difference between an optimisation and a guard, and the reason the fix
 * for the flood is here while the fix for the emission is there.
 */
static int scsd_joiner_retransmit_pending(const struct scsd_rx *rx,
                                          const struct peer_state *ps)
{
    if (!(rx != NULL && rx->do_connect && ps != NULL && ps->pb != NULL &&
          ps->start_acked && ps->joiner_connect_sent && !ps->joiner_connected)) {
        return 0;
    }
    struct scs_config_path_info path;
    return scs_config_path(ps->pb, &path) && path.vc_state == SCS_VC_OPEN;
}

/*
 * ===========================================================================
 * vms-66f: THE SCS PROCESS POLLER (SCS$DIR_LOOKUP), ON THE WIRE
 * ===========================================================================
 *
 * The poller itself is src/vmsscs/scs_poll.c (p. 2-50: the cadence, the
 * one-node-at-a-time rule, the notify-on-Yes-only rule, the per-(SYSAP,node)
 * disable). What lives HERE is only the port-driver half: turning the two
 * actions it needs into the two frames scs_dir.c builds, finding the peer the
 * frames ride to, and feeding received answers back in.
 *
 * WHAT THIS DOES *NOT* DO, AND WHY -- READ BEFORE "FINISHING THE JOB".
 * The item that produced this file was written expecting the poller to REPLACE
 * a speculative client walk: OVMX opening a VMS$VAXcluster connection without
 * first asking whether the target is listening. Two facts, both checked rather
 * than assumed, changed the shape of the change:
 *
 *  (1) NO SUCH WALK EXISTS ON THIS BASE. Before this item, scsd.c never sent an
 *      outbound SCS$DIRECTORY connect at all -- scs_dir.c held three templates
 *      and every one of them was a RESPONSE. OVMX could answer a lookup and
 *      could not make one. There was nothing to delete.
 *
 *  (2) THE REFERENCE JOINER POLLS *AFTER* IT IS IN, NOT BEFORE.
 *
 *      CORRECTED (vms-66f, adversary-caught): an earlier revision of this
 *      comment said --
 *      REFUTED-QUOTE-BEGIN
 *        "the directory exchange runs in ONE direction ... VAX2 only
 *        answers. VAX2 then opens its own VMS$VAXcluster connection WITHOUT
 *        having polled anybody."
 *      REFUTED-QUOTE-END
 *      Both of those are REFUTED by the capture they
 *      cite, and the roles were inverted. Re-derive with
 *      tools/cluster/scs_dir_role_measure.py (14 checks, 0 failures,
 *      2026-08-05); frame numbers below are 0-based raw pcap indices of
 *      formation-ci1-joinwindow.pcap:
 *
 *        - VAX2 NEVER sends a VMS$VAXcluster CONNECT_REQ. The file holds
 *          exactly ONE ([46:48]=0, name pair VMS$VAXcluster/VMS$VAXcluster) and
 *          it is frame 47, VAX1 -> VAX2. VAX2's answer, frame 50, VAX2 -> VAX1,
 *          is [46:48]=2, an ACCEPT_REQ. The ESTABLISHED MEMBER is the active half.
 *        - VAX2 DOES poll: frame 1237, VAX2 -> VAX1, [46:48]=0, name pair
 *          ("SCS$DIRECTORY", "SCS$DIR_LOOKUP"), with its own lookup round
 *          following on it (1244/1248 asking, 1247/1250 answering).
 *
 *      What survives is ORDERING: VAX2's poll is at t+33.804 s, 0.36 s AFTER
 *      the VMS$VAXcluster connection formed at t+33.444 s. A joiner polls once
 *      it is in. So gating OVMX's joiner connect on a poller answer still moves
 *      OVMX away from the reference wire -- but because of the SEQUENCE, not
 *      because the joiner never polls. That connect is left exactly as it was.
 *      Full census and both refuted sentences: spec sec 4(h)(2a).
 *
 * What the poller adds is the discovery path OVMX genuinely lacked: a node OVMX
 * can see but which has never opened a directory connection TO it was, until
 * now, a node whose SYSAPs OVMX could never learn about. That is the gap
 * p. 2-50 exists to fill, and it is what is wired below.
 *
 * KILL-SWITCH: OVMX_NO_PROCESS_POLLER=1. It gates scs_poll_tick(), which is the
 * only thing that starts a cycle, so with it set neither frame below is ever
 * built. Measured, not asserted -- see the item's bracket.
 */
/* The poller's emitters need the socket and the peer table; both live in
 * struct scsd_rx, so that is what the ctx is. */
static struct scsd_rx *scsd_poll_rx = NULL;

/* Find the peer whose SCS System Address is `node` (the CONFIG_SYS key the
 * poller passes as args->target_node). NULL if the peer is gone. */
static struct peer_state *scsd_peer_by_sys(struct scsd_rx *rx, const uint8_t node[6])
{
    if (rx == NULL || node == NULL) {
        return NULL;
    }
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        struct peer_state *ps = &rx->peers[i];
        if (ps->pb != NULL && memcmp(ps_sys_addr(ps), node, 6) == 0) {
            return ps;
        }
    }
    return NULL;
}

/*
 * peer_by_csid - the live peer whose DLM CSID (peer_node_number(), the low 16
 * bits of its SCA src-logical address) is target_csid. NULL if no such peer is
 * live. rd vms-dca9 (DLM rung H10b): the REMASTER REBUILD send is the first DLM
 * sender that must reach a peer OTHER than the one whose join tick drove the
 * call -- the resource's NEW directory master, not the departed one -- so it
 * looks the target up BY CSID, mirroring scsd_peer_by_sys's by-identity lookup
 * immediately above.
 */
static struct peer_state *peer_by_csid(struct scsd_rx *rx, uint32_t target_csid)
{
    if (rx == NULL || target_csid == 0) {
        return NULL;
    }
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        struct peer_state *ps = &rx->peers[i];
        if (ps->pb != NULL && (uint32_t)peer_node_number(ps) == target_csid) {
            return ps;
        }
    }
    return NULL;
}

/* Fill the shared envelope every poller frame needs from a peer. */
static void scsd_poll_dir_params(struct scsd_rx *rx, struct peer_state *ps,
                                 struct scs_dir_params *dp)
{
    memset(dp, 0, sizeof(*dp));
    memcpy(dp->dst_mac, ps_port_addr(ps), 6);
    memcpy(dp->src_mac, rx->our_hw_mac, 6);
    memcpy(dp->src_logical, rx->our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
    memcpy(dp->peer_logical, ps_sys_addr(ps), 6);
    dp->local_conid = PS_SCS_DIR_POLL_CONID(ps);
    dp->incarnation = ps->incarnation; /* sec 4i echo, same rule as the responder */
    dp->recv_ack = ps->vc.seq.recv_seq;
    dp->send_seq = scs_seq_advance(&ps->vc.seq);
}

/*
 * CONNECT / DISCONNECT for the poller's own connection. Goes through
 * send_frame_vc(), the p. 2-31 choke point, so this is a CHOKED entry in the
 * SEND SITE TABLE and takes no exemption.
 *
 * ROUND 4 -- THE TEARDOWN THIS EMITTER USED TO DECLINE TO SEND. The first
 * revision answered NOBUILDER for SEND_DISCONNECT_REQ and said so in a comment
 * citing scs_svc.h. That was true when it was written and became FALSE under
 * the rebase that put vms-591 in this file's base: scs_disc_build_request()
 * exists (src/vmsscs/scs_disc.c) and scsd_svc_emit_disconnect() above already
 * drives it for the receive path and the shutdown teardown. The comment was
 * therefore not stale prose -- it was a capability the poller DECLINED TO USE
 * because a comment said it did not exist, and the measurable consequence was
 * that the p. 2-50 cycle ("the Process Poller and Directory Service disconnect
 * from each other") could never complete: every cycle ended by force-releasing
 * its descriptor (scs_poll.c descriptors_forced) and both clean-release arms of
 * the poller were unreachable code. The DISCONNECT arm below is that fix.
 *
 * ONE EMITTER, NOT TWO. The frame is built by scsd_svc_emit_disconnect(), the
 * SAME function the receive path and the shutdown teardown use, with a ctx
 * built from the peer the poller's cycle is addressed to. A second DISCONNECT
 * builder here would be a second thing to keep byte-identical, and it would sit
 * outside the SEND SITE TABLE entry that already covers that emitter.
 *
 * NOT HANDLED HERE, DELIBERATELY: SEND_DISCONNECT_RSP. That action appears only
 * on a RECEIVE arrow of Figure 2-16, and the poller drives no receive events --
 * scsd_handle_frame() feeds a peer's DISCONNECT messages to
 * scsd_disconnect_dialogue(), which carries its own emitter. Wiring an
 * unreachable RSP arm here would be untestable code, so it falls to the
 * no-builder answer below with everything else the poller can be asked for.
 */
static int scsd_poll_emit(void *ctx, struct scs_cdt *cdt, enum scs_conn_action act,
                          const struct scs_svc_args *args, const char **what)
{
    struct scsd_rx *rx = (struct scsd_rx *)ctx;
    if (act == SCS_CONN_ACT_SEND_DISCONNECT_REQ) {
        struct peer_state *dps = scsd_peer_by_sys(rx, args->target_node);
        struct scsd_disc_emit_ctx e;
        int r;
        if (dps == NULL) {
            return SCS_SVC_EMIT_REFUSED;
        }
        memset(&e, 0, sizeof(e));
        e.sock = rx->sock;
        e.ifindex = rx->ifindex;
        e.ps = dps;
        e.our_hw_mac = rx->our_hw_mac;
        e.our_src_logical = rx->our_src_logical;
        /* p. 2-26's reason code is OPTIONAL and this disconnect has no reason
         * to give: the cycle ended because every inquiry was answered, which is
         * success. SCS_REASON_NONE is what every VMS-origin DISCONNECT_REQ we
         * hold carries (scs_reason.h), and it is what scs_svc_args zeroes to.
         * e.matching stays 0 -- the poller INITIATES this dialogue. */
        e.reason = args->reason;
        r = scsd_svc_emit_disconnect(&e, cdt, act, args, what);
        if (r == SCS_SVC_EMIT_SENT) {
            rx->poll_disconnect_sent++;
            log_ts(stdout);
            printf(" SCSD-I-POLLDISC, SCS$DIR_LOOKUP disconnects from"
                   " SCS$DIRECTORY on %02x:%02x:%02x:%02x:%02x:%02x"
                   " local_conid=0x%08X (p. 2-50: all replies in)\n",
                   ps_port_addr(dps)[0], ps_port_addr(dps)[1], ps_port_addr(dps)[2],
                   ps_port_addr(dps)[3], ps_port_addr(dps)[4], ps_port_addr(dps)[5],
                   (unsigned)PS_SCS_DIR_POLL_CONID(dps));
            fflush(stdout);
        }
        return r;
    }
    if (act != SCS_CONN_ACT_SEND_CONNECT_REQ) {
        /* ACCEPT_RSP (Figure 2-14's answer to the directory's ACCEPT_REQ) is
         * the one action the poller reaches here in a normal cycle, and OVMX
         * has no builder for it -- scs_svc.h's list, which no longer names the
         * two DISCONNECT frames. Say so rather than pretending. */
        return scsd_svc_no_builder(cdt, act);
    }
    struct peer_state *ps = scsd_peer_by_sys(rx, args->target_node);
    if (ps == NULL) {
        return SCS_SVC_EMIT_REFUSED;
    }
    struct scs_dir_params dp;
    scsd_poll_dir_params(rx, ps, &dp);
    uint8_t frame[SCS_DIR_CONNREQ_FRAME_LEN];
    if (scs_dir_build_connect_request(&dp, frame) != 0) {
        return SCS_SVC_EMIT_REFUSED;
    }
    *what = "SCS$DIR_LOOKUP -> SCS$DIRECTORY CONNECT-REQUEST";
    if (send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb, *what,
                      frame, sizeof(frame)) <= 0) {
        return SCS_SVC_EMIT_REFUSED;
    }
    rx->poll_connect_sent++;
    log_ts(stdout);
    printf(" SCSD-I-POLLCONN, SCS$DIR_LOOKUP connects to SCS$DIRECTORY on"
           " %02x:%02x:%02x:%02x:%02x:%02x local_conid=0x%08X seq=%u\n",
           ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
           ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
           (unsigned)PS_SCS_DIR_POLL_CONID(ps), dp.send_seq);
    fflush(stdout);
    return SCS_SVC_EMIT_SENT;
}

/* ONE inquiry: "is <sysap> in your list of listening SYSAPs?" (p. 2-50). */
static int scsd_poll_inquire(void *ctx, struct scs_cdt *cdt, const uint8_t node[6],
                             const char *sysap)
{
    struct scsd_rx *rx = (struct scsd_rx *)ctx;
    (void)cdt;
    struct peer_state *ps = scsd_peer_by_sys(rx, node);
    if (ps == NULL) {
        return 0;
    }
    struct scs_dir_params dp;
    scsd_poll_dir_params(rx, ps, &dp);

    struct scs_dir_lookup_params lp;
    memset(&lp, 0, sizeof(lp));
    memcpy(lp.dst_mac, dp.dst_mac, 6);
    memcpy(lp.src_mac, dp.src_mac, 6);
    memcpy(lp.src_logical, dp.src_logical, 6);
    memcpy(lp.peer_logical, dp.peer_logical, 6);
    lp.remote_conid = ps->poll_remote_conid;
    lp.local_conid = PS_SCS_DIR_POLL_CONID(ps);
    lp.recv_ack = dp.recv_ack;
    lp.send_seq = dp.send_seq;
    lp.incarnation = dp.incarnation;
    /* Once the connection is up the exchange uses 0x4b (spec sec 4h / 4g
     * phase-3), which is what SCA#37 shows the reference poller doing. */
    lp.opcode = SCS_MSGTYPE_SEQAPP;
    lp.op = SCS_DIR_OP_LOOKUP;
    memset(lp.name, ' ', sizeof(lp.name));
    size_t nl = strlen(sysap);
    if (nl > SCS_DIR_NAME_LEN) {
        nl = SCS_DIR_NAME_LEN;
    }
    memcpy(lp.name, sysap, nl);

    uint8_t frame[SCS_DIR_LOOKUP_FRAME_LEN];
    if (scs_dir_build_lookup_request(&lp, frame) != 0) {
        return 0;
    }
    if (send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                      "SCS$DIR_LOOKUP inquiry", frame, sizeof(frame)) <= 0) {
        return 0;
    }
    rx->poll_inquiry_sent++;
    log_ts(stdout);
    printf(" SCSD-I-POLLASK, SCS$DIR_LOOKUP asks %02x:%02x:%02x:%02x:%02x:%02x"
           " about '%s' seq=%u\n",
           ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
           ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
           sysap, lp.send_seq);
    fflush(stdout);
    return 1;
}

/*
 * p. 2-50's "SYSAP_A is notified about the discovery of SYSAP_X; and SYSAP_A
 * can then connect to SYSAP_X." OVMX's interested SYSAP is its connection
 * manager, so the notification opens the VMS$VAXcluster connection -- the SAME
 * call the grounded post-directory path makes, from a different trigger.
 */
static void scsd_poll_found(const char *sysap, const uint8_t node[6], void *ctx)
{
    struct scsd_rx *rx = (struct scsd_rx *)ctx;
    struct peer_state *ps = scsd_peer_by_sys(rx, node);
    if (ps == NULL) {
        return;
    }
    log_ts(stdout);
    printf(" SCSD-I-POLLFOUND, SCS$DIR_LOOKUP discovered '%s' listening on"
           " %02x:%02x:%02x:%02x:%02x:%02x -- notifying the SYSAP\n",
           sysap, ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
           ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5]);
    fflush(stdout);
    rx->poll_found++;
    if (!ps->joiner_connected &&
        send_joiner_connect_request(rx->sock, rx->ifindex, rx->cfg, ps, NULL,
                                    rx->our_hw_mac, rx->our_src_logical)) {
        rx->connect_req_sent++;
        log_ts(stdout);
        printf(" SCSD-I-CONNREQ, sent OUR VMS$VAXcluster CONNECT-REQUEST"
               " local_conid=0x%08X seq=%u (poller discovery)\n",
               PS_JOINER_CONID(ps), ps->joiner_req_seq);
        fflush(stdout);
    }
}

/*
 * The poller, bound on first use for exactly the reason scsd_svc() is: the
 * SCSD_UNIT_TEST seam renames main() away, so anything bound only in main()
 * would be dead in every wire test.
 */
static struct scs_poller *scsd_poll(struct scsd_rx *rx)
{
    if (!scsd_poller_ready) {
        scsd_poller_ready = 1;
        scsd_poll_rx = rx;
        scs_poll_init(&scsd_poller, scsd_svc(), rx->cfg);
        scs_poll_set_emitters(&scsd_poller, scsd_poll_emit, rx,
                              scsd_poll_inquire, rx);
        /*
         * p. 2-50: "VMS software, in fact, requests polling for SYSAP names on
         * all nodes." OVMX registers the ONE SYSAP it would actually connect to
         * on discovery -- its connection manager. Registering names OVMX would
         * do nothing about (MSCP$DISK, MSCP$TAPE) would put inquiries on the
         * wire whose answers no code consumes, which is noise, not discovery.
         */
        (void)scs_poll_request(&scsd_poller, "VMS$VAXcluster", NULL,
                               scsd_poll_found, rx);
    }
    return &scsd_poller;
}

/* =====================================================================
 * vms-578 INTEGRATION: the worktree-760 SEND-SIDE HELPERS, MOVED.
 * =====================================================================
 * These are worktree-760's join-sequencer / pure-server-disk-client /
 * connection-manager senders, verbatim apart from three mechanical changes:
 *   - ps->eth_mac  -> ps_port_addr(ps)   (the PB owns the port address, p. 2-12)
 *   - ps->logical  -> ps_sys_addr(ps)    (the SB owns the system address, p. 2-16)
 *   - send_frame_to() -> send_frame_vc() (THE CHOKE POINT; see the SEND SITE
 *     TABLE. worktree-760's send_frame_to was a second raw transmit primitive
 *     and is deleted -- every one of these is a sequenced message on a formed
 *     circuit, so all of them are CHOKED, none is exempt.)
 *
 * They sat BELOW the receive loop on worktree-760, where an inline loop could
 * call them without declarations. scsd_handle_frame() is a function, so they
 * move above it rather than acquire a block of forward declarations.
 */

/*
 * send_mscp_connect_request - vms-760: send OVMX's MSCP$DISK client
 * CONNECT-REQUEST (VMS$DISK_CL_DRVR -> MSCP$DISK) to the member, remote=0,
 * offering OVMX_MSCP_CONID. The clean joiner opens this disk-client connection
 * BEFORE its VMS$VAXcluster VC (clean-ref MSCP idx35 precedes VC idx47); the
 * live member requires the joiner to present it before it reciprocates the
 * add-member config (NEW->MEMBER, spec sec 4L(7)). Rides the SAME shared
 * per-channel SCS send_seq as every sequenced send: first send allocates a
 * send_seq; retransmits REUSE it (spec sec 4L(4)). Returns 1 if a frame sent.
 */
static int send_mscp_connect_request(int sock, int ifindex, struct peer_state *ps,
                                     const uint8_t our_hw_mac[6],
                                     const uint8_t our_src_logical[6])
{
    struct scs_connect_params cp;
    memset(&cp, 0, sizeof(cp));
    memcpy(cp.dst_mac, ps_port_addr(ps), 6);
    memcpy(cp.src_mac, our_hw_mac, 6);
    memcpy(cp.src_logical, our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
    memcpy(cp.peer_logical, ps_sys_addr(ps), 6);
    cp.local_conid = PS_MSCP_CONID(ps);
    cp.remote_conid = 0; /* CONNECT-REQUEST: the member's Con.ID is not yet known */
    cp.recv_ack = ps->vc.seq.recv_seq; /* always ack the member's latest send_seq */
    if (ps->mscp_req_seq == 0) {
        ps->mscp_req_seq = scs_seq_advance(&ps->vc.seq); /* allocate once */
    }
    cp.send_seq = ps->mscp_req_seq; /* retransmits reuse the same seq */
    cp.incarnation = ps->incarnation;
    uint8_t cframe[SCS_CONNECT_FRAME_LEN];
    /* vms-760: connect-class [8:10] (abs22) = 0x0001, NOT the template's 0x03e8.
     * GROUNDED on the definitive 2->3 reference (vax3-2to3-established-join): the real
     * joiner's MSCP$DISK connect is byte-identical to ours EXCEPT this field. With
     * 0x03e8 the established member echoes nothing and never op2-accepts, which is the
     * long-standing "MSCP connect not accepted" stall. */
    if (scs_connect_build_mscp_request(&cp, cframe) == 0 &&
        (cframe[22] = 0x01, cframe[23] = 0x00, 1) &&
        send_frame_vc(sock, ifindex, ps, ps->pb, "VMS$VAXcluster CONNECT-REQUEST (joiner)", cframe, sizeof(cframe)) > 0) {
        ps->mscp_connect_sent = 1;
        clock_gettime(CLOCK_MONOTONIC, &ps->last_mscp_req);
        scs_vc_record_sent(&ps->vc, cp.send_seq, monotonic_ms());
        return 1;
    }
    return 0;
}

/*
 * ps_mscp_disc - vms-760: drive MSCP disk DISCOVERY on OUR bound MSCP$DISK client
 * connection. GROUNDED on vax3-2to3-established-join: the joiner issues SET CONTROLLER
 * CHARACTERISTICS (op 0x04) TWICE, then walks GET UNIT STATUS (op 0x03, NEXT-UNIT
 * modifier) with unit = previous END's returned unit-word + 1, until an END returns
 * MSCP status OFFLINE (0x0003) = end-of-list. VAX1 reciprocates the add-member config
 * (-> op 0x03 COMMIT -> MEMBER) only after this completes. `which` selects the next
 * command from peer state. Returns 1 if a frame was sent.
 */
static int ps_mscp_disc(int sock, int ifindex, struct peer_state *ps,
                        const uint8_t our_hw_mac[6],
                        const uint8_t our_src_logical[6])
{
    if (!ps->mscp_connected || ps->mscp_disc_done) {
        return 0;
    }
    struct scs_mscp_params mp;
    memset(&mp, 0, sizeof(mp));
    memcpy(mp.dst_mac, ps_port_addr(ps), 6);
    memcpy(mp.src_mac, our_hw_mac, 6);
    memcpy(mp.src_logical, our_src_logical, 6);
    memcpy(mp.peer_logical, ps_sys_addr(ps), 6);
    mp.remote_conid = ps->mscp_remote_conid;
    mp.local_conid = PS_MSCP_CONID(ps);
    mp.recv_ack = ps->vc.seq.recv_seq;
    mp.send_seq = scs_seq_advance(&ps->vc.seq);
    mp.incarnation = ps->incarnation;

    uint8_t f[SCS_MSCP_FRAME_LEN];
    int ok;
    if (ps->mscp_scc_sent < 2) {
        mp.cmd_ref = SCS_MSCP_CMD_REF(
            SCS_MSCP_SCC_CLASS, (uint16_t)(SCS_MSCP_SCC_MSGID0 + ps->mscp_scc_sent));
        mp.unit = 0;
        ok = (scs_mscp_build_scc(&mp, f) == 0);
        if (ok && send_frame_vc(sock, ifindex, ps, ps->pb, "sequenced SCS message", f, sizeof(f)) > 0) {
            ps->mscp_scc_sent++;
            scs_vc_record_sent(&ps->vc, mp.send_seq, monotonic_ms());
            return 1;
        }
        return 0;
    }
    mp.cmd_ref = SCS_MSCP_CMD_REF(
        SCS_MSCP_GUS_CLASS, (uint16_t)(SCS_MSCP_GUS_MSGID0 + ps->mscp_msg_id));
    /* GROUNDED (vax3-2to3): the FIRST GUS seeds unit-word 0x0001; each subsequent
     * command uses the previous END's returned unit-word + 1. Seeding 0 makes VAX1
     * answer OFFLINE immediately and the walk ends after one exchange. */
    mp.unit = (ps->mscp_msg_id == 0) ? 0x0001 : ps->mscp_next_unit;
    ok = (scs_mscp_build_gus(&mp, f) == 0);
    if (ok && send_frame_vc(sock, ifindex, ps, ps->pb, "sequenced SCS message", f, sizeof(f)) > 0) {
        ps->mscp_msg_id++;
        scs_vc_record_sent(&ps->vc, mp.send_seq, monotonic_ms());
        return 1;
    }
    return 0;
}

/*
 * send_own_dir_connect_request - vms-760: open OVMX's OWN SCS$DIRECTORY
 * connection to the member (the active joiner's idx25). A real joiner opens its
 * own directory connection and queries the member; OVMX previously only ANSWERED
 * the member's directory. remote=0 (member's handle not yet known), local =
 * SCS_DIR_OVMX_JOINER_CONID. One sequenced message; retransmits reuse the seq.
 */
static int send_own_dir_connect_request(int sock, int ifindex, struct peer_state *ps,
                                        const uint8_t our_hw_mac[6],
                                        const uint8_t our_src_logical[6])
{
    struct scs_dir_params dp;
    memset(&dp, 0, sizeof(dp));
    memcpy(dp.dst_mac, ps_port_addr(ps), 6);
    memcpy(dp.src_mac, our_hw_mac, 6);
    memcpy(dp.src_logical, our_src_logical, 6);
    memcpy(dp.peer_logical, ps_sys_addr(ps), 6);
    dp.local_conid = PS_SCS_DIR_JOINER_CONID(ps);
    dp.remote_conid = 0;
    dp.recv_ack = ps->vc.seq.recv_seq;
    if (ps->own_dir_req_seq == 0) {
        ps->own_dir_req_seq = scs_seq_advance(&ps->vc.seq);
    }
    dp.send_seq = ps->own_dir_req_seq;
    dp.incarnation = ps->incarnation;
    uint8_t f[SCS_DIR_RESP_FRAME_LEN];
    if (scs_dir_build_connect_request(&dp, f) == 0 &&
        send_frame_vc(sock, ifindex, ps, ps->pb, "sequenced SCS message", f, sizeof(f)) > 0) {
        ps->own_dir_sent = 1;
        clock_gettime(CLOCK_MONOTONIC, &ps->last_own_dir);
        scs_vc_record_sent(&ps->vc, dp.send_seq, monotonic_ms());
        return 1;
    }
    return 0;
}

/*
 * send_own_dir_lookup - vms-760: query the member's directory for a SYSAP `name`
 * on OUR directory connection. The FIRST send of a lookup (retx=0) allocates a new
 * shared send_seq and stores it + the name in the sequencer (js_lookup_seq/
 * js_lookup_name) for stop-and-wait retransmit; a retransmit (retx=1) REUSES that
 * stored seq + name -- a retransmit is not a new message, and advancing the seq
 * would open the 760mscp hole (spec sec 4L(4)). Returns 1 if a frame was sent.
 */
static int send_own_dir_lookup(int sock, int ifindex, struct peer_state *ps,
                               const uint8_t our_hw_mac[6],
                               const uint8_t our_src_logical[6],
                               const char *name, int retx)
{
    struct scs_dir_lookup_params lp;
    memset(&lp, 0, sizeof(lp));
    memcpy(lp.dst_mac, ps_port_addr(ps), 6);
    memcpy(lp.src_mac, our_hw_mac, 6);
    memcpy(lp.src_logical, our_src_logical, 6);
    memcpy(lp.peer_logical, ps_sys_addr(ps), 6);
    lp.remote_conid = ps->own_dir_remote_conid; /* member's handle on OUR dir connection */
    lp.local_conid = PS_SCS_DIR_JOINER_CONID(ps);
    lp.recv_ack = ps->vc.seq.recv_seq;
    if (retx) {
        lp.send_seq = ps->js_lookup_seq;       /* REUSE the outstanding seq */
        strncpy(lp.name, ps->js_lookup_name, SCS_DIR_NAME_LEN - 1);
    } else {
        lp.send_seq = scs_seq_advance(&ps->vc.seq);
        ps->js_lookup_seq = lp.send_seq;       /* store for retransmit reuse */
        memset(ps->js_lookup_name, 0, sizeof(ps->js_lookup_name));
        strncpy(ps->js_lookup_name, name, SCS_DIR_NAME_LEN - 1);
        strncpy(lp.name, name, SCS_DIR_NAME_LEN - 1);
    }
    lp.incarnation = ps->incarnation;
    uint8_t f[SCS_DIR_LOOKUP_FRAME_LEN];
    if (scs_dir_build_lookup_request(&lp, f) != 0) {
        return 0;
    }
    /* vms-760: once the MSCP$DISK connection is bound the joiner's remaining dir
     * lookups switch to DATA-PHASE framing -- msgtype 0x4b and flag[48] (abs62) = 1.
     * GROUNDED on the 2->3 reference (vax3-2to3-established-join): VAX3's MSCP$TAPE /
     * 1st MSCP$DISK lookups are 0x5b/flag0, then its 2nd MSCP$DISK and VMS$VAXcluster
     * lookups are 0x4b/flag1. Sending the VMS$VAXcluster lookup as 0x5b/flag0 leaves
     * the member silent (the step-6 stall). */
    if (ps->mscp_connected) {
        f[30] = SCS_MSGTYPE_SEQAPP;
        f[62] = 0x01;
    }
    if (send_frame_vc(sock, ifindex, ps, ps->pb, "sequenced SCS message", f, sizeof(f)) > 0) {
        scs_vc_record_sent(&ps->vc, lp.send_seq, monotonic_ms());
        clock_gettime(CLOCK_MONOTONIC, &ps->js_last_tx);
        return 1;
    }
    return 0;
}

/*
 * send_own_dir_confirm - vms-760: send the op=3 dir CONNECT-CONFIRM on OUR
 * directory connection, the third frame of the joiner's own SCS$DIRECTORY client
 * handshake (clean-ref SCA idx26). Fire-and-forget: the member bare-ACKs it (0x48)
 * but sends no response, so the sequencer does NOT wait on it. Advances the shared
 * send_seq once. Returns 1 if a frame was sent.
 */
static int send_own_dir_confirm(int sock, int ifindex, struct peer_state *ps,
                                const uint8_t our_hw_mac[6],
                                const uint8_t our_src_logical[6])
{
    struct scs_dir_params dp;
    memset(&dp, 0, sizeof(dp));
    memcpy(dp.dst_mac, ps_port_addr(ps), 6);
    memcpy(dp.src_mac, our_hw_mac, 6);
    memcpy(dp.src_logical, our_src_logical, 6);
    memcpy(dp.peer_logical, ps_sys_addr(ps), 6);
    dp.remote_conid = ps->own_dir_remote_conid;
    dp.local_conid = PS_SCS_DIR_JOINER_CONID(ps);
    dp.recv_ack = ps->vc.seq.recv_seq;
    dp.send_seq = scs_seq_advance(&ps->vc.seq);
    dp.incarnation = ps->incarnation;
    uint8_t f[SCS_DIR_CONFIRM_FRAME_LEN];
    if (scs_dir_build_connect_confirm(&dp, f) == 0 &&
        send_frame_vc(sock, ifindex, ps, ps->pb, "sequenced SCS message", f, sizeof(f)) > 0) {
        scs_vc_record_sent(&ps->vc, dp.send_seq, monotonic_ms());
        return 1;
    }
    return 0;
}

/* ======================================================================
 * vms-760 PURE-SERVER disk-CLIENT send helpers. Each rides the SHARED
 * per-channel ps->vc.seq (contiguous with every server-side send). Connect +
 * lookup seqs are allocate-once / retransmit-reuse (never advance on a
 * retransmit -- that is the 760mscp hole); confirm + MSCP data frames advance
 * once. These use the DISTINCT PS conids (OVMX_PS_DIR_CONID / OVMX_PS_MSCP_CONID)
 * so the OVMX_JOIN_SEQ receive handlers never match them. All are gated by the
 * caller on getenv("OVMX_PURE_SERVER"). NONE opens a VMS$VAXcluster VC.
 * ====================================================================== */

/* Open OUR OWN PS SCS$DIRECTORY connection to VAX1 (remote=0, local=PS dir). */
static int ps_send_dir_connect(int sock, int ifindex, struct peer_state *ps,
                               const uint8_t our_hw_mac[6],
                               const uint8_t our_src_logical[6])
{
    struct scs_dir_params dp;
    memset(&dp, 0, sizeof(dp));
    memcpy(dp.dst_mac, ps_port_addr(ps), 6);
    memcpy(dp.src_mac, our_hw_mac, 6);
    memcpy(dp.src_logical, our_src_logical, 6);
    memcpy(dp.peer_logical, ps_sys_addr(ps), 6);
    dp.local_conid = PS_PS_DIR_CONID(ps);
    dp.remote_conid = 0;
    dp.recv_ack = ps->vc.seq.recv_seq;
    if (ps->psc_dir_req_seq == 0) {
        ps->psc_dir_req_seq = scs_seq_advance(&ps->vc.seq); /* allocate once */
    }
    dp.send_seq = ps->psc_dir_req_seq; /* retransmits reuse the same seq */
    dp.incarnation = ps->incarnation;
    uint8_t f[SCS_DIR_RESP_FRAME_LEN];
    /* vms-760: OVMX's VMS$VAXcluster VC is established by now (OVMX reached NEW), so
     * OVMX's OWN client connects are data-phase (msgtype 0x4b), NOT the 0x5b the
     * builder emits for fresh VC formation. GROUNDED: af2's joiner opens its dir +
     * MSCP client connects with 0x4b post-NEW; VAX1 ECHOES a 0x5b connect (op=1) but
     * never op=2-accepts it -> the client connection never binds. */
    if (scs_dir_build_connect_request(&dp, f) == 0 &&
        (f[30] = SCS_MSGTYPE_SEQAPP, 1) &&
        send_frame_vc(sock, ifindex, ps, ps->pb, "sequenced SCS message", f, sizeof(f)) > 0) {
        ps->psc_dir_sent = 1;
        clock_gettime(CLOCK_MONOTONIC, &ps->psc_last_tx);
        scs_vc_record_sent(&ps->vc, dp.send_seq, monotonic_ms());
        return 1;
    }
    return 0;
}

/* Send the op=3 CONNECT-CONFIRM on OUR PS dir connection (fire-and-forget). */
static int ps_send_dir_confirm(int sock, int ifindex, struct peer_state *ps,
                               const uint8_t our_hw_mac[6],
                               const uint8_t our_src_logical[6])
{
    struct scs_dir_params dp;
    memset(&dp, 0, sizeof(dp));
    memcpy(dp.dst_mac, ps_port_addr(ps), 6);
    memcpy(dp.src_mac, our_hw_mac, 6);
    memcpy(dp.src_logical, our_src_logical, 6);
    memcpy(dp.peer_logical, ps_sys_addr(ps), 6);
    dp.remote_conid = ps->psc_dir_remote_conid;
    dp.local_conid = PS_PS_DIR_CONID(ps);
    dp.recv_ack = ps->vc.seq.recv_seq;
    dp.send_seq = scs_seq_advance(&ps->vc.seq);
    dp.incarnation = ps->incarnation;
    uint8_t f[SCS_DIR_CONFIRM_FRAME_LEN];
    if (scs_dir_build_connect_confirm(&dp, f) == 0 &&
        (f[30] = SCS_MSGTYPE_SEQAPP, 1) &&   /* post-NEW: data-phase 0x4b */
        send_frame_vc(sock, ifindex, ps, ps->pb, "sequenced SCS message", f, sizeof(f)) > 0) {
        scs_vc_record_sent(&ps->vc, dp.send_seq, monotonic_ms());
        return 1;
    }
    return 0;
}

/* Query VAX1's directory for `name` on OUR PS dir connection. First send (retx=0)
 * allocates + stores the shared send_seq and name; a retransmit (retx=1) REUSES
 * them. Returns 1 if a frame was sent. */
static int ps_send_dir_lookup(int sock, int ifindex, struct peer_state *ps,
                              const uint8_t our_hw_mac[6],
                              const uint8_t our_src_logical[6],
                              const char *name, int retx)
{
    struct scs_dir_lookup_params lp;
    memset(&lp, 0, sizeof(lp));
    memcpy(lp.dst_mac, ps_port_addr(ps), 6);
    memcpy(lp.src_mac, our_hw_mac, 6);
    memcpy(lp.src_logical, our_src_logical, 6);
    memcpy(lp.peer_logical, ps_sys_addr(ps), 6);
    lp.remote_conid = ps->psc_dir_remote_conid;
    lp.local_conid = PS_PS_DIR_CONID(ps);
    lp.recv_ack = ps->vc.seq.recv_seq;
    if (retx) {
        lp.send_seq = ps->psc_lookup_seq;
        strncpy(lp.name, ps->psc_lookup_name, SCS_DIR_NAME_LEN - 1);
    } else {
        lp.send_seq = scs_seq_advance(&ps->vc.seq);
        ps->psc_lookup_seq = lp.send_seq;
        memset(ps->psc_lookup_name, 0, sizeof(ps->psc_lookup_name));
        strncpy(ps->psc_lookup_name, name, SCS_DIR_NAME_LEN - 1);
        strncpy(lp.name, name, SCS_DIR_NAME_LEN - 1);
    }
    lp.incarnation = ps->incarnation;
    uint8_t f[SCS_DIR_LOOKUP_FRAME_LEN];
    if (scs_dir_build_lookup_request(&lp, f) == 0 &&
        (f[30] = SCS_MSGTYPE_SEQAPP, 1) &&   /* post-NEW: data-phase 0x4b */
        send_frame_vc(sock, ifindex, ps, ps->pb, "sequenced SCS message", f, sizeof(f)) > 0) {
        scs_vc_record_sent(&ps->vc, lp.send_seq, monotonic_ms());
        clock_gettime(CLOCK_MONOTONIC, &ps->psc_last_tx);
        return 1;
    }
    return 0;
}

/* Open OUR OWN PS MSCP$DISK client connection (remote=0, local=PS mscp). */
static int ps_send_mscp_connect(int sock, int ifindex, struct peer_state *ps,
                                const uint8_t our_hw_mac[6],
                                const uint8_t our_src_logical[6])
{
    struct scs_connect_params cp;
    memset(&cp, 0, sizeof(cp));
    memcpy(cp.dst_mac, ps_port_addr(ps), 6);
    memcpy(cp.src_mac, our_hw_mac, 6);
    memcpy(cp.src_logical, our_src_logical, 6);
    memcpy(cp.peer_logical, ps_sys_addr(ps), 6);
    cp.local_conid = PS_PS_MSCP_CONID(ps);
    cp.remote_conid = 0;
    cp.recv_ack = ps->vc.seq.recv_seq;
    if (ps->psc_mscp_req_seq == 0) {
        ps->psc_mscp_req_seq = scs_seq_advance(&ps->vc.seq); /* allocate once */
    }
    cp.send_seq = ps->psc_mscp_req_seq; /* retransmits reuse */
    cp.incarnation = ps->incarnation;
    uint8_t f[SCS_CONNECT_FRAME_LEN];
    /* vms-760: post-NEW client MSCP connect -> data-phase msgtype 0x4b AND connect-class
     * [8:10]=0x0001 (GROUNDED af2), NOT the builder's 0x5b / 0x03e8 fresh-formation values. */
    if (scs_connect_build_mscp_request(&cp, f) == 0 &&
        (f[30] = SCS_MSGTYPE_SEQAPP, f[22] = 0x01, f[23] = 0x00, 1) &&
        send_frame_vc(sock, ifindex, ps, ps->pb, "sequenced SCS message", f, sizeof(f)) > 0) {
        ps->psc_mscp_sent = 1;
        clock_gettime(CLOCK_MONOTONIC, &ps->psc_last_tx);
        scs_vc_record_sent(&ps->vc, cp.send_seq, monotonic_ms());
        return 1;
    }
    return 0;
}

/* Send the op=3 CONNECT-CONFIRM on OUR PS MSCP$DISK connection (structurally the
 * generic op=3 dir confirm, but on the MSCP conid pair). Fire-and-forget. */
static int ps_send_mscp_confirm(int sock, int ifindex, struct peer_state *ps,
                                const uint8_t our_hw_mac[6],
                                const uint8_t our_src_logical[6])
{
    struct scs_dir_params dp;
    memset(&dp, 0, sizeof(dp));
    memcpy(dp.dst_mac, ps_port_addr(ps), 6);
    memcpy(dp.src_mac, our_hw_mac, 6);
    memcpy(dp.src_logical, our_src_logical, 6);
    memcpy(dp.peer_logical, ps_sys_addr(ps), 6);
    dp.remote_conid = ps->psc_mscp_remote_conid;
    dp.local_conid = PS_PS_MSCP_CONID(ps);
    dp.recv_ack = ps->vc.seq.recv_seq;
    dp.send_seq = scs_seq_advance(&ps->vc.seq);
    dp.incarnation = ps->incarnation;
    uint8_t f[SCS_DIR_CONFIRM_FRAME_LEN];
    if (scs_dir_build_connect_confirm(&dp, f) == 0 &&
        (f[30] = SCS_MSGTYPE_SEQAPP, 1) &&   /* post-NEW: data-phase 0x4b */
        send_frame_vc(sock, ifindex, ps, ps->pb, "sequenced SCS message", f, sizeof(f)) > 0) {
        scs_vc_record_sent(&ps->vc, dp.send_seq, monotonic_ms());
        return 1;
    }
    return 0;
}

/*
 * cm_send_barrier_step - send our op-0x0b request for barrier step N on the VC
 * the connection-manager dialogue is riding. See scs_member_build_barrier().
 */
static int cm_send_barrier_step(int sock, int ifindex, struct peer_state *ps,
                                const uint8_t our_hw_mac[6],
                                const uint8_t our_src_logical[6], int step)
{
    if (ps->cm_local_conid == 0) {
        return 0;
    }
    struct scs_member_params bp;
    memset(&bp, 0, sizeof(bp));
    memcpy(bp.dst_mac, ps_port_addr(ps), 6);
    memcpy(bp.src_mac, our_hw_mac, 6);
    memcpy(bp.src_logical, our_src_logical, 6);
    memcpy(bp.peer_logical, ps_sys_addr(ps), 6);
    bp.remote_conid = ps->cm_remote_conid;
    bp.local_conid = ps->cm_local_conid;
    bp.incarnation = ps->incarnation;
    bp.recv_ack = ps->vc.seq.recv_seq;
    bp.send_seq = scs_seq_advance(&ps->vc.seq);
    if (ps->sysap_send == 0) {
        ps->sysap_send = 1;
    }
    bp.sysap_send_msg = ps->sysap_send++;
    bp.sysap_ack_msg = ps->sysap_recv;
    if (ps->own_txn == 0) {
        ps->own_txn = 1;   /* our per-VC transaction context */
    }
    bp.txn = ps->own_txn;
    bp.checksum = ++ps->own_cksum; /* one counter for every transaction we start */
    uint8_t bframe[SCS_MEMBER_FRAME_LEN];
    if (scs_member_build_barrier(&bp, ps->barrier_epoch, (uint32_t)step, bframe) == 0 &&
        send_frame_vc(sock, ifindex, ps, ps->pb, "CM barrier step (cat 0x04)", bframe, sizeof(bframe)) > 0) {
        scs_vc_record_sent(&ps->vc, bp.send_seq, monotonic_ms());
        log_ts(stdout);
        printf(" SCSD-I-BARRIER, state-transition barrier step %d/%d"
               " (epoch=0x%08X txn=%u cksum=0x%04x)\n",
               step, SCS_MEMBER_BARRIER_STEPS, (unsigned)ps->barrier_epoch,
               bp.txn, bp.checksum);
        fflush(stdout);
        return 1;
    }
    return 0;
}

/*
 * cm_send_ack - emit one category-0x04 SYSAP acknowledgement naming our current
 * high-water mark on the VC the connection-manager dialogue is riding.
 *
 * Called from two places, which together reproduce what VMS does:
 *   - the receive path, once every SCS_CM_ACK_EVERY unacked messages (the
 *     reference joiner answers a 254-message burst with 85 acks); and
 *   - the poll loop, to FLUSH a residual backlog that never reaches the
 *     threshold. Batching without a flush strands the coordinator's last one or
 *     two messages and the transaction stops dead (d94-e6).
 * Acking every message instead is safe but starves our own response path: with
 * 260 acks in flight, token-correlated requests waited 32 ms for their answer
 * where a real VAX answers in well under 2 ms (d94-e8 frames 1378-1402).
 */
static int cm_send_ack(int sock, int ifindex, struct peer_state *ps,
                       const uint8_t our_hw_mac[6], const uint8_t our_src_logical[6])
{
    if (ps->cm_local_conid == 0) {
        return 0;
    }
    struct scs_member_params ap;
    memset(&ap, 0, sizeof(ap));
    memcpy(ap.dst_mac, ps_port_addr(ps), 6);
    memcpy(ap.src_mac, our_hw_mac, 6);
    memcpy(ap.src_logical, our_src_logical, 6);
    memcpy(ap.peer_logical, ps_sys_addr(ps), 6);
    ap.remote_conid = ps->cm_remote_conid;
    ap.local_conid = ps->cm_local_conid;
    ap.incarnation = ps->incarnation;
    ap.recv_ack = ps->vc.seq.recv_seq;
    ap.send_seq = scs_seq_advance(&ps->vc.seq);
    if (ps->sysap_send == 0) {
        ps->sysap_send = 1;
    }
    ap.sysap_send_msg = ps->sysap_send++;
    ap.sysap_ack_msg = ps->sysap_recv;
    uint8_t aframe[SCS_MEMBER_FRAME_LEN];
    if (scs_member_build_ack(&ap, aframe) == 0 &&
        send_frame_vc(sock, ifindex, ps, ps->pb, "CM cat-0x04 ack", aframe, sizeof(aframe)) > 0) {
        scs_vc_record_sent(&ps->vc, ap.send_seq, monotonic_ms());
        ps->sysap_acked = ps->sysap_recv;
        ps->cm_last_ack_ms = monotonic_ms();
        ps->cm_acks++;
        /* vms-584 STRAY-ACK INSTRUMENTATION. A 26-capture census of the
         * reference gives a rule we do NOT currently match:
         *
         *   A member's cat-0x04 ack is PROMPT (0.30/0.53/0.39/0.45 ms after the
         *   frame it names), OPPORTUNISTIC (no timer, no fixed N: idle captures
         *   carry zero acks, busy links show 0 ms-2.3 s gaps with no period),
         *   CUMULATIVE, and never keyed to an opcode -- it names whatever was
         *   genuinely received last. An ack of an `op 0x01` occurs 0/4 times in
         *   the reference; an ack-of-ack occurs once, as an amsg coincidence.
         *
         * OVMX emits two shapes the reference does not: an ack naming an
         * `op 0x01` ~7.0 s after it arrived (by10 idx 255, by11 idx 2945,
         * bystander idx 254 -- 7013/6754/7014 ms), and a genuine ack-of-ack ~4 s
         * late (by11 idx 3006, bystander idx 4922). Both are provably INERT: no
         * peer in any run reacted to either, and OVMX's own op 0x02 landed in the
         * same instant and was acked normally 2.5 ms later.
         *
         * So this LOGS rather than GATES, deliberately. The emission rule above
         * carries its own grounded claim -- that only the op-0x06 burst is
         * acked, and that no cat-0x04 ack in the library is attributable to the
         * op 0x0a / op 0x0c notifications -- and widening the trigger to "ack
         * whatever advanced the high-water mark" would contradict it on the one
         * path that is currently working. Guard 8: a guard that hides a bug is
         * worse than no guard, and a change that silences an inert divergence
         * while risking the join is worse than measuring it. The next capture
         * now says WHICH frame each stray named and how stale it was. */
        long age = monotonic_ms() - ps->cm_recv_advanced_ms;
        if (ps->cm_recv_advanced_ms != 0 &&
            (age >= 1000 || ps->cm_last_recv_cat == SCS_MEMBER_CAT_ACK)) {
            log_ts(stdout);
            printf(" SCSD-W-STRAYACK, cat-0x04 ack names msg#%u whose frame"
                   " (cat 0x%02x op 0x%02x) arrived %ld ms ago -- the reference"
                   " acks within ~1 ms and never acks an op 0x01%s\n",
                   ps->sysap_recv, ps->cm_last_recv_cat, ps->cm_last_recv_op,
                   age, ps->cm_last_recv_cat == SCS_MEMBER_CAT_ACK
                        ? "; this one is an ACK-OF-ACK" : "");
            fflush(stdout);
        }
        return 1;
    }
    return 0;
}

/* vms-a58 RULING -- WHAT THIS FUNCTION IS HALF OF, AND WHAT IS MISSING.
 *
 * The "op8" this answers is SCS message type 8 at SCA [46:48]. It is now
 * measured rather than guessed (tools/cluster/scs_t89_measure.py, spec
 * sec 4(h)(1f) + sec 5; `ctest -R scs_t89_figures` re-derives it):
 *
 *   T89-CENSUS-INV: t8_sender_is_disconnect_initiator=131
 *   T89-CENSUS-INV: ovmx_disconnect_req_rank0=0
 *
 * The first says the node that sends the type 8 is the node that then sends the
 * DISCONNECT_REQ, in every dialogue in the lab-1 library, with nothing but the
 * type 9 in between -- the 8/9 exchange is biconditional with teardown and
 * always opened by the disconnecting end. The second says OVMX has never
 * INITIATED a teardown on any capture we hold; every DISCONNECT_REQ it has ever
 * emitted is the matching half. THAT is the only reason answering (below) has
 * been enough so far.
 *
 * SO: the moment OVMX initiates a teardown -- which vms-591's
 * scs_disc_build_request() and vms-66f's poller wiring made possible -- it will
 * send a DISCONNECT_REQ that no type 8 preceded, violating a 131-of-131 wire
 * invariant. Emitting the type 8 first is REQUIRED, is NOT implemented here,
 * and is a first-class candidate for the vms-abd "Inappropriate SCA Control
 * Message" refusal. When it is built: derive [48:50] from this connection's own
 * outstanding-credit count (scs_credit.c) -- do NOT hard-code the 1 the
 * captures show, which is an artifact of the strictly alternating
 * SCS$DIRECTORY workload and not a constant of the message. And do not NAME the
 * message: types 8 and 9 are grounded in position, pairing and credit
 * behaviour, and in nothing else (spec sec 5 lists the four eliminated
 * readings, including the p. 2-44 special credit message).
 *
 * vms-760: SCS connection-management credit/ready handshake. After a connection
 * binds (op0/1/2/3), VAX1 runs op8->op9 and op6->op7 control exchanges on it, and
 * GATES further admission (incl. accepting the joiner's own client connects) on the
 * joiner answering them. GROUNDED af2 143.759 (member's dir conn) + 143.893 (joiner's):
 * the answer is a standard connection-response reflection -- swap eth+cluster-logical,
 * op -> op+1 (6->7 / 8->9), swap the Con.ID pair (rc<->lc), advance the shared VC seq.
 * op6 is a 62-byte SCA request; op7/op9 are 58-byte (72-byte frame) -- we emit 72. */
static int scs_reflect_credit(int sock, int ifindex, struct peer_state *ps,
                              const uint8_t our_hw_mac[6],
                              const uint8_t our_src_logical[6],
                              const uint8_t *buf, size_t n)
{
    if (n < 72) {
        return 0;
    }
    uint8_t r[72];
    memcpy(r, buf, 72);                        /* base structure (drops op6's tail 4B) */
    memcpy(r + 0, ps_port_addr(ps), 6);             /* eth dst = VAX1 */
    memcpy(r + 6, our_hw_mac, 6);              /* eth src = OVMX */
    memcpy(r + 14 + 2, ps_sys_addr(ps), 6);        /* dst cluster-logical = VAX1 (abs16) */
    memcpy(r + 14 + 10, our_src_logical, 6);   /* src cluster-logical = OVMX (abs24) */
    uint16_t rseq = (uint16_t)buf[34] | ((uint16_t)buf[35] << 8); /* their send_seq */
    /* vms-2f3 sec 4M.20: ALLOCATE-ONCE / RETRANSMIT-REUSE. If this request
     * carries a send_seq we have already answered, replay the reply we sent
     * then -- do NOT consume a fresh sequence number. Measured on the wire
     * (Q1B, VAX1 link): the peer replayed op8 send_seq=12 three times and OVMX
     * answered send_seq 12, then 13, then 14, manufacturing two phantom
     * messages in the VC stream. The struct header states this rule for
     * connect/lookup seqs and psc_dir_req_seq already implements it; this path
     * was the hole, and it only became reachable when sec 4M.14 started
     * answering retransmissions at all.
     * OVMX_CREDIT_NO_SEQ_REUSE=1 restores the always-advance behaviour. */
    uint16_t sseq;
    if (getenv("OVMX_CREDIT_NO_SEQ_REUSE") != NULL) {
        sseq = scs_seq_advance(&ps->vc.seq);
    } else {
        sseq = scs_retx_reply_seq(&ps->credit_seq, &ps->vc.seq, rseq);
    }
    /* recv_ack @abs 32/40/48, send_seq @abs 34/44 (dir_build_common layout). */
    r[32] = (uint8_t)rseq; r[33] = (uint8_t)(rseq >> 8);
    r[40] = (uint8_t)rseq; r[41] = (uint8_t)(rseq >> 8);
    r[48] = (uint8_t)rseq; r[49] = (uint8_t)(rseq >> 8);
    r[34] = (uint8_t)sseq; r[35] = (uint8_t)(sseq >> 8);
    r[44] = (uint8_t)sseq; r[45] = (uint8_t)(sseq >> 8);
    /* vms-a63 (spec sec 4(i).B / 4(O.19)/4(O.20)) -- THE REJOIN FREEZE. Stamp
     * OVMX's OWN node-incarnation at [22:24] (abs 36). content[22:24] is the
     * node-incarnation counter, GROUNDED byte-exact in sec 4(i).B across the
     * af2 first-timer specimens (1->2->3), and it is a per-SOURCE-NODE constant
     * over every capture we hold: in the SUCCESS rejoin oracle
     * (vax3-class03-crash-REJOIN-SUCCESS) the rejoiner stamps 2 on 10495/10495
     * of its frames -- op-6/7/8/9 and application MTYPE-10 alike -- and the two
     * established members stamp 1 on 8216/8216 of theirs; it never varies by
     * peer, op, or seq. It is the SAME [22:24] echo the vms-691 fix already
     * stamps on the 0x41 START via every envelope builder (ps_fill_mscp,
     * scs_member/scs_connect/scs_dir all write put_le16(out+14+22,
     * ps->incarnation)).
     *
     * The memcpy above inherits the op-8 REQUEST's [36:38]. An op-8 FROM the
     * coordinator carries the COORDINATOR's incarnation (1), so OVMX's op-9
     * reply -- which must carry OVMX's incarnation (2 on a rejoin, advertised
     * back to OVMX in the member's directed-HELLO [78:80] and read into
     * ps->incarnation) -- went out stamped 1. A rejoining node that advertises
     * incarnation 2 on every envelope frame but 1 on its op-9 (ss=11) presents
     * an inconsistent incarnation; the coordinator FREEZES its per-VC recv_seq
     * at ss=10 and never advances past that one frame (sec 4(O.19): op-9 ss=11,
     * op-6 ss=12 and the deferred op 0x02 ss=13 all ride behind the freeze ->
     * cm_responses=0, XITDONE=0). Derive, never inherit -- the identical vms-760
     * rule the length words below already follow. Measured live at vaxlab-0:
     * OVMX stamps 2 on its op-0/1/2/3/10 but 1 on op-9 (the frozen frame).
     * OVMX_CREDIT_NO_INCARN_ECHO=1 restores the inherited value (the control
     * arm: recv_seq stays frozen at 10). */
    if (getenv("OVMX_CREDIT_NO_INCARN_ECHO") == NULL) {
        uint16_t inc = ps->incarnation ? ps->incarnation : 1u;
        r[36] = (uint8_t)inc; r[37] = (uint8_t)(inc >> 8);
    }
    r[60] = (uint8_t)(buf[60] + 1);            /* op 6->7 / 8->9 */
    r[61] = 0;
    /* vms-2f3 sec 4M.18: the memcpy above inherits the REQUEST's msgtype at
     * abs 30. That is harmless while the request is 0x4b/0x5b, but once sec
     * 4M.14 made us answer the RETRANSMIT form 0x7b, our op7/op9 reply started
     * going out marked 0x7b as well -- i.e. announcing itself as a
     * retransmission of a frame we had never sent. Observed directly on the
     * wire (Q1B +28.582: `OVMX->PEER mt=7b op=9`).
     *
     * Every op7 and op9 in the capture library is 0x4b, including the ones a
     * real node sends. Emit the data-phase form always. This is the same
     * "derive, never inherit" rule as the length words below -- and note it is
     * NOT the sec 4M.12 mirroring question, which is about lookup RESPONSES and
     * remains an open RE gap (vms-7e7). Here the corpus is unanimous.
     *
     * OVMX_CREDIT_MIRROR_MSGTYPE=1 restores the inherited msgtype. */
    if (getenv("OVMX_CREDIT_MIRROR_MSGTYPE") == NULL) {
        r[30] = SCS_MSGTYPE_SEQAPP;            /* 0x4b, never 0x7b */
    }
    memcpy(r + 64, buf + 68, 4);               /* rc = their lc (address the requester) */
    memcpy(r + 68, buf + 64, 4);               /* lc = their rc (our conid) */

    /* vms-760 -- THE STALL. Both LENGTH words must describe the frame we are
     * ACTUALLY emitting, not the one we are answering. The memcpy above inherits
     * them from the request; for op8 (a 58-byte SCA, like our reply) that happens
     * to be right, but an op6 request is a 62-byte SCA, so our 58-byte op7 went
     * out declaring 4 bytes it did not carry.
     *
     * GROUNDED INVARIANT, vax3-2to3-established-join-20260730.pcap: across 13556
     * audited SCS frames (both peers, whole capture) declared[abs14] ==
     * payload_len - 2 and inner[abs56] == payload_len - 44, with ZERO exceptions;
     * every op7 in the capture carries exactly 56/14 over a 72-byte frame.
     *
     * The member validates it. Our over-declared op7 was dropped as a runt, so the
     * member's recv_seq stayed put, the next (well-formed) frame arrived with a
     * sequence gap and was dropped too, its directory connection was left
     * disconnect-pending -- and because MSCP$DISK is opened only AFTER that
     * teardown completes, it never opened one to us. Reproduced identically
     * against all three VAXes in d94-vcfix/vcfix2/cm4. Derive, never inherit. */
    uint16_t sca_len = (uint16_t)(sizeof(r) - 14 - 2); /* 56 */
    uint16_t inner_len = (uint16_t)(sizeof(r) - 58);   /* 14 */
    r[14] = (uint8_t)sca_len;   r[15] = (uint8_t)(sca_len >> 8);
    r[56] = (uint8_t)inner_len; r[57] = (uint8_t)(inner_len >> 8);

    return send_frame_vc(sock, ifindex, ps, ps->pb, "CM transition response (hand-built)", r, sizeof(r)) > 0;
}

/* vms-760: send OVMX's OWN op6 DISCONNECT-REQUEST to complete the bidirectional
 * teardown of a transient directory connection. GROUNDED af2 143.76011: after acking
 * VAX1's op6 (with op7), the joiner sends its own op6 (76-byte, 62-B SCA, tail
 * 00 00 01 00) and VAX1 acks op7 -> the connection is fully closed, and ONLY THEN does
 * the joiner open its own client dir connect. Built from the received VAX1 op6 frame
 * (`buf`, 76 bytes): reflect identity + swap the Con.ID pair, op stays 6, keep the tail. */
/* vms-2f3 sec 4M.23: byte-exact 76-byte op6 DISCONNECT-REQUEST as OVMX itself
 * emits it in a SUCCESSFUL join, lifted from our own lab wire
 * (d94-Q1A.pcap, OVMX->VAX2). Used as the template for an OVMX-INITIATED
 * disconnect, where there is no received op6 to copy. Identity, Con.IDs and
 * counters are all substituted; the tail 00 00 01 00 is the value already
 * GROUNDED at af2 143.76011 for a joiner's own op6. Rule 8: observed on our own
 * wire, not derived from any VSI/HPE source. */
static const uint8_t scs_dir_disc_tmpl[76] = {
    /* [0:6]   */ 0x08, 0x00, 0x2b, 0x1e, 0x85, 0x61,   /* eth dst  (SUBST) */
    /* [6:12]  */ 0x1e, 0x1f, 0xeb, 0x2f, 0x07, 0x08,   /* eth src  (SUBST) */
    /* [12:14] */ 0x60, 0x07,
    /* [14:16] */ 0x3c, 0x00,                           /* SCA length 60 (DERIVED below) */
    /* [16:22] */ 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,   /* dst logical (SUBST) */
    /* [22:24] */ 0x01, 0x00,                           /* connect flag */
    /* [24:30] */ 0xaa, 0x00, 0x04, 0x00, 0x21, 0x05,   /* src logical (SUBST) */
    /* [30:32] */ 0x4b, 0x13,                           /* msgtype 0x4b / format 0x13 */
    /* [32:34] */ 0x0e, 0x00,                           /* recv_ack (SUBST) */
    /* [34:36] */ 0x0f, 0x00,                           /* send_seq (SUBST) */
    /* [36:38] */ 0x01, 0x00,
    /* [38:40] */ 0x12, 0x00,
    /* [40:42] */ 0x0e, 0x00,                           /* recv_ack mirror (SUBST) */
    /* [42:44] */ 0x00, 0x00,
    /* [44:46] */ 0x0f, 0x00,                           /* send_seq mirror (SUBST) */
    /* [46:48] */ 0x00, 0x00,
    /* [48:50] */ 0x0e, 0x00,                           /* recv_ack 3rd (SUBST) */
    /* [50:52] */ 0x00, 0x00,
    /* [52:56] */ 0x01, 0x00, 0x00, 0x02,
    /* [56:58] */ 0x12, 0x00,                           /* inner length 18 (DERIVED below) */
    /* [58:60] */ 0x04, 0x00,
    /* [60:62] */ 0x06, 0x00,                           /* op = 6, DISCONNECT-REQUEST */
    /* [62:64] */ 0x00, 0x00,
    /* [64:68] */ 0x0f, 0x00, 0x00, 0x6f,               /* remote Con.ID (SUBST) */
    /* [68:72] */ 0x07, 0x00, 0x20, 0x77,               /* local Con.ID  (SUBST) */
    /* [72:76] */ 0x00, 0x00, 0x01, 0x00,               /* tail, GROUNDED af2 143.76011 */
};

/* vms-2f3 sec 4M.23: perform OVMX's OWN disconnect call when the peer's op6
 * never arrives.
 *
 * GROUNDED IN PUBLIC DOCUMENTATION -- Digital Technical Journal Vol.1 No.5
 * (Sept 1987), "The System Communication Architecture", p.25: "that member
 * performs a disconnect call to its SCA software. The SCA software will inform
 * the SYSAP in the other node, WHICH MUST THEN PERFORM ITS OWN DISCONNECT CALL
 * to synchronize the dismantling of the connection." The teardown is SYMMETRIC.
 *
 * On a rejoin the peer's op6 never arrives -- its CDT sits in
 * disc_sent/disc_pend (sec 4M.18) -- so OVMX never performs its own disconnect
 * call and by the documented protocol the teardown cannot complete.
 *
 * CORRECTED (vms-096): this comment used to say "OVMX only ever emitted op6 in
 * REPLY to the peer's (scs_send_disconnect, from the cm_op==6 branch)". That
 * branch was nested inside `cm_op == 8` and was unreachable, so on THIS branch
 * OVMX never emitted that reply at all; scs_send_disconnect() is deleted. The
 * architected reply to an op 6 is scs_disc_build_response(), driven by the
 * vms-591/vms-dd5 path -- which does not change the sentence below, because the
 * peer's op 6 is what never arrives.
 * This performs it on the same 2000 ms timeout that already fires PSCUNGATE. */
static int scs_send_disconnect_self(int sock, int ifindex, struct peer_state *ps,
                                    const uint8_t our_hw_mac[6],
                                    const uint8_t our_src_logical[6])
{
    uint8_t d[76];
    memcpy(d, scs_dir_disc_tmpl, sizeof(d));
    memcpy(d + 0, ps_port_addr(ps), 6);
    memcpy(d + 6, our_hw_mac, 6);
    memcpy(d + 14 + 2, ps_sys_addr(ps), 6);
    memcpy(d + 14 + 10, our_src_logical, 6);

    uint16_t rseq = ps->vc.seq.recv_seq;
    uint16_t sseq = scs_seq_advance(&ps->vc.seq);
    d[32] = (uint8_t)rseq; d[33] = (uint8_t)(rseq >> 8);
    d[40] = (uint8_t)rseq; d[41] = (uint8_t)(rseq >> 8);
    d[48] = (uint8_t)rseq; d[49] = (uint8_t)(rseq >> 8);
    d[34] = (uint8_t)sseq; d[35] = (uint8_t)(sseq >> 8);
    d[44] = (uint8_t)sseq; d[45] = (uint8_t)(sseq >> 8);

    /* vms-a63: stamp OVMX's node-incarnation at [22:24] (abs 36), same rule as
     * scs_reflect_credit -- scs_dir_disc_tmpl hard-codes [36:38]=0x0100 (=1),
     * but a rejoining OVMX must present its own incarnation (2). Derive, never
     * inherit. OVMX_CREDIT_NO_INCARN_ECHO=1 restores the template value. */
    if (getenv("OVMX_CREDIT_NO_INCARN_ECHO") == NULL) {
        uint16_t inc = ps->incarnation ? ps->incarnation : 1u;
        d[36] = (uint8_t)inc; d[37] = (uint8_t)(inc >> 8);
    }

    /* Con.ID pair: remote = the peer's handle on OUR server dir connection,
     * local = ours. */
    uint32_t rc = ps->dir_remote_conid;
    uint32_t lc = (uint32_t)PS_SCS_DIR_CONID(ps);
    d[64] = (uint8_t)rc;  d[65] = (uint8_t)(rc >> 8);
    d[66] = (uint8_t)(rc >> 16); d[67] = (uint8_t)(rc >> 24);
    d[68] = (uint8_t)lc;  d[69] = (uint8_t)(lc >> 8);
    d[70] = (uint8_t)(lc >> 16); d[71] = (uint8_t)(lc >> 24);

    /* DERIVE the length words from what we emit -- never inherit (the vms-760
     * stall, and the same rule scs_reflect_credit documents below). */
    uint16_t sca_len = (uint16_t)(sizeof(d) - 14 - 2); /* 60 */
    uint16_t inner_len = (uint16_t)(sizeof(d) - 58);   /* 18 */
    d[14] = (uint8_t)sca_len;   d[15] = (uint8_t)(sca_len >> 8);
    d[56] = (uint8_t)inner_len; d[57] = (uint8_t)(inner_len >> 8);

    return send_frame_vc(sock, ifindex, ps, ps->pb, "CM self-disconnect (hand-built)", d, sizeof(d)) > 0;
}

/* vms-46f (spec 4(O.17)): INITIATE OVMX's own op-6 special-credit request on the
 * coordinator's SCS$DIRECTORY connection during a REJOIN, ahead of the deferred
 * op 0x02.
 *
 * WHY. On a rejoin the coordinator is SEND-CREDIT-STARVED (Davis pp. 2-43/2-45):
 * OVMX never returns Pending Receive Credit, so the coordinator's Send Credit to
 * OVMX is never replenished and it cannot transmit its op 0x04 reciprocation
 * (Credit Wait) -- cm_responses=0, XITDONE=0. The SUCCESS oracle sends its op-6
 * (f1261, ss=13) BEFORE op 0x02 (f1297, ss=14); OVMX deferred its op-6 to
 * ss=14/15. This function emits that op-6 up front so the ordering matches.
 *
 * THE FRAME is byte-for-byte OVMX's own grounded op-6 on the SCS$DIRECTORY
 * connection (scs_dir_disc_tmpl, observed on our own wire at d94-Q1A;
 * Rule 8 -- not derived from any VSI/HPE source). The op field at abs[60] is 6.
 * The Pending Receive Credit that this exchange returns is banked separately by
 * the caller (scs_credit_release_buffer on the directory CDT) so the following
 * op 0x02 (a 190-byte MTYPE-10, a credit class) piggybacks it at abs[48:50] --
 * the mechanism the coordinator needs to recover Send Credit. Whether that ALONE
 * completes readmission is what the live bracket answers (spec 4(O.17)). */
static int scs_send_rejoin_credit_op6(int sock, int ifindex, struct peer_state *ps,
                                      const uint8_t our_hw_mac[6],
                                      const uint8_t our_src_logical[6])
{
    uint8_t d[76];
    memcpy(d, scs_dir_disc_tmpl, sizeof(d));
    memcpy(d + 0, ps_port_addr(ps), 6);
    memcpy(d + 6, our_hw_mac, 6);
    memcpy(d + 14 + 2, ps_sys_addr(ps), 6);
    memcpy(d + 14 + 10, our_src_logical, 6);

    uint16_t rseq = ps->vc.seq.recv_seq;
    uint16_t sseq = scs_seq_advance(&ps->vc.seq);
    d[32] = (uint8_t)rseq; d[33] = (uint8_t)(rseq >> 8);
    d[40] = (uint8_t)rseq; d[41] = (uint8_t)(rseq >> 8);
    d[48] = (uint8_t)rseq; d[49] = (uint8_t)(rseq >> 8);
    d[34] = (uint8_t)sseq; d[35] = (uint8_t)(sseq >> 8);
    d[44] = (uint8_t)sseq; d[45] = (uint8_t)(sseq >> 8);

    /* vms-a63: stamp OVMX's node-incarnation at [22:24] (abs 36) -- this op-6
     * rides the coordinator VC at ss=12, right behind the op-9 ss=11 freeze
     * frame (sec 4(O.19)); once the op-9 fix unfreezes recv_seq to 11, this op-6
     * must ALSO carry the correct incarnation or it re-freezes at 11. Derive,
     * never inherit; OVMX_CREDIT_NO_INCARN_ECHO=1 restores the template value. */
    if (getenv("OVMX_CREDIT_NO_INCARN_ECHO") == NULL) {
        uint16_t inc = ps->incarnation ? ps->incarnation : 1u;
        d[36] = (uint8_t)inc; d[37] = (uint8_t)(inc >> 8);
    }

    /* Con.ID pair: remote = the coordinator's handle on OUR server dir
     * connection, local = ours (the same 0x0C640007/0x8DE4000D pair the freeze
     * forensic named, spec 4(O.17)). */
    uint32_t rc = ps->dir_remote_conid;
    uint32_t lc = (uint32_t)PS_SCS_DIR_CONID(ps);
    d[64] = (uint8_t)rc;  d[65] = (uint8_t)(rc >> 8);
    d[66] = (uint8_t)(rc >> 16); d[67] = (uint8_t)(rc >> 24);
    d[68] = (uint8_t)lc;  d[69] = (uint8_t)(lc >> 8);
    d[70] = (uint8_t)(lc >> 16); d[71] = (uint8_t)(lc >> 24);

    /* DERIVE the length words -- never inherit (the vms-760 stall rule). */
    uint16_t sca_len = (uint16_t)(sizeof(d) - 14 - 2); /* 60 */
    uint16_t inner_len = (uint16_t)(sizeof(d) - 58);   /* 18 */
    d[14] = (uint8_t)sca_len;   d[15] = (uint8_t)(sca_len >> 8);
    d[56] = (uint8_t)inner_len; d[57] = (uint8_t)(inner_len >> 8);

    return send_frame_vc(sock, ifindex, ps, ps->pb,
                         "rejoin op-6 special-credit request (spec 4(O.17))",
                         d, sizeof(d)) > 0;
}

/* scs_send_disconnect() STOOD HERE AND IS DELETED (vms-096).
 *
 * It echoed the peer's op-6 frame back with the Con.ID pair swapped, and its
 * ONE call site was the `cm_op == 6` block nested inside `cm_op == 8` -- i.e.
 * unreachable, so no OVMX build since the vms-578 integration has ever emitted
 * this frame. PROOF, not assertion:
 *   - static: cm_op is a single local `uint16_t`, assigned once from
 *     buf[60..61] and never written again in the enclosing block, so
 *     `cm_op == 6` inside `if (cm_op == 8 && ...)` is a contradiction;
 *   - census: over the six vaxlab-4 captures produced by this branch, OVMX
 *     emits msgtype-6 frames only from scs_disc_build_request() (the vms-591
 *     architected builder) -- their 62-byte DISCONNECT_REQ class, never this
 *     function's 76-byte hand-built one.
 * tests/vmsscs/test_scsd_send_sites.py now refuses a `cm_op == 6` comparison
 * anywhere in scsd.c, which is the negative control that reds if the block is
 * re-added.
 *
 * Nothing is lost architecturally: op 6 IS the DISCONNECT_REQUEST of spec sec
 * 4(h)(1a), and answering it is scs_disc_build_response()'s job, driven off the
 * CDT by the vms-dd5 classifier. scs_send_disconnect_self() above is a
 * different thing and is KEPT -- it performs OVMX's OWN disconnect call on the
 * op-8 stall signal, opt-in behind OVMX_DIR_SELF_DISCONNECT.
 */

/* Fill the shared MSCP command envelope (identity + Con.ID pair + live counters
 * + correlation token) for a PS disk-client command. */
static void ps_fill_mscp(const struct peer_state *ps, const uint8_t our_hw_mac[6],
                         const uint8_t our_src_logical[6],
                         uint32_t cmd_ref, uint16_t unit,
                         uint16_t send_seq, struct scs_mscp_params *mp)
{
    memset(mp, 0, sizeof(*mp));
    memcpy(mp->dst_mac, ps_port_addr(ps), 6);
    memcpy(mp->src_mac, our_hw_mac, 6);
    memcpy(mp->src_logical, our_src_logical, 6);
    memcpy(mp->peer_logical, ps_sys_addr(ps), 6);
    mp->remote_conid = ps->psc_mscp_remote_conid;
    mp->local_conid = PS_PS_MSCP_CONID(ps);
    mp->recv_ack = ps->vc.seq.recv_seq;
    mp->send_seq = send_seq;
    mp->incarnation = ps->incarnation;
    mp->cmd_ref = cmd_ref;
    mp->unit = unit;
}

/* Send a SET CONTROLLER CHARACTERISTICS command on OUR PS MSCP connection (the
 * FIRST MSCP message; opcode 0x04). Advances the shared send_seq once. */
static int ps_send_scc(int sock, int ifindex, struct peer_state *ps,
                       const uint8_t our_hw_mac[6],
                       const uint8_t our_src_logical[6])
{
    if (ps->psc_mscp_msgid == 0) {
        ps->psc_mscp_msgid = SCS_MSCP_SCC_MSGID0;
    }
    struct scs_mscp_params mp;
    uint16_t seq = scs_seq_advance(&ps->vc.seq);
    ps_fill_mscp(ps, our_hw_mac, our_src_logical,
                 SCS_MSCP_CMD_REF(SCS_MSCP_SCC_CLASS, ps->psc_mscp_msgid),
                 0, seq, &mp);
    uint8_t f[SCS_MSCP_FRAME_LEN];
    if (scs_mscp_build_scc(&mp, f) == 0 &&
        send_frame_vc(sock, ifindex, ps, ps->pb, "sequenced SCS message", f, sizeof(f)) > 0) {
        ps->psc_scc_sent++;
        ps->psc_mscp_msgid++; /* next command's correlation token */
        clock_gettime(CLOCK_MONOTONIC, &ps->psc_last_tx);
        scs_vc_record_sent(&ps->vc, seq, monotonic_ms());
        return 1;
    }
    return 0;
}

/* Send a GET UNIT STATUS command for `unit` on OUR PS MSCP connection (opcode
 * 0x03, NEXT-UNIT modifier). Advances the shared send_seq once. */
static int ps_send_gus(int sock, int ifindex, struct peer_state *ps,
                       const uint8_t our_hw_mac[6],
                       const uint8_t our_src_logical[6], uint16_t unit)
{
    if (ps->psc_mscp_msgid == 0 || ps->psc_gus_sent == 0) {
        /* GUS uses its own correlation-token stream, seeded from the grounded
         * af2 GUS message-id; the SCC stream ends when GUS begins. */
        ps->psc_mscp_msgid = (uint16_t)(SCS_MSCP_GUS_MSGID0 + ps->psc_gus_sent);
    }
    struct scs_mscp_params mp;
    uint16_t seq = scs_seq_advance(&ps->vc.seq);
    ps_fill_mscp(ps, our_hw_mac, our_src_logical,
                 SCS_MSCP_CMD_REF(SCS_MSCP_GUS_CLASS, ps->psc_mscp_msgid),
                 unit, seq, &mp);
    uint8_t f[SCS_MSCP_FRAME_LEN];
    if (scs_mscp_build_gus(&mp, f) == 0 &&
        send_frame_vc(sock, ifindex, ps, ps->pb, "sequenced SCS message", f, sizeof(f)) > 0) {
        ps->psc_gus_sent++;
        ps->psc_mscp_msgid++;
        clock_gettime(CLOCK_MONOTONIC, &ps->psc_last_tx);
        scs_vc_record_sent(&ps->vc, seq, monotonic_ms());
        return 1;
    }
    return 0;
}

/*
 * scsd_handle_frame - dispatch ONE received Ethernet frame. `n` is the length
 * recv() returned; the caller has already handled the error returns. This is
 * the daemon's entire receive path.
 */
/*
 * ===== vms-7c0: THE VMS$VAXcluster SYSAP'S p. 2-29 MESSAGE INPUT ROUTINE =====
 *
 * "The port driver then offsets to a fixed location within the CDT to locate
 *  the address of the remote SYSAP's message input routine, and passes the
 *  message to that routine."                                        (p. 2-29)
 * "the message and datagram input routines stored in the CDT [are] supplied as
 *  arguments to the CONNECT and ACCEPT services."                   (p. 2-29)
 *
 * This is the routine scs_cdt.h said OVMX did not have. It is installed on
 * EVERY CDT the five services open (all four scs_svc_args sites below set
 * .msg_input to it), and it is reached only from scs_cdl_deliver_message(),
 * which is reached only from the CDL lookup in scsd_handle_frame().
 *
 * Its body is the vms-224 connection-manager add-member dialogue, moved here
 * WHOLE and otherwise unchanged. The one thing that changed is the question it
 * no longer has to ask: it used to test the frame's Con.ID pair against
 * OVMX_LOCAL_CONID / OVMX_JOINER_CONID to find out whether the frame was for
 * OVMX and, if so, which circuit it rode. The CDL has answered both by the time
 * this runs -- the frame is here BECAUSE its destination Con.ID resolved to
 * `cdt` -- so `cm_on_joiner_vc` is now a pointer comparison against the CDT the
 * joiner connection owns.
 *
 * WHAT THAT CHANGES IN BEHAVIOUR, stated rather than glossed:
 *   - a frame whose destination Con.ID names a connection that does not exist,
 *     or has been released, is no longer handled (counted: rx_deliver_no_cdt).
 *     The old macro test could not tell a released Con.ID from a live one.
 *   - a frame whose SOURCE Con.ID is not the peer handle this connection was
 *     bound to is refused by p. 2-35 (counted: rx_deliver_src_mismatch). A
 *     returning incarnation of the peer issues new Con.IDs, and the old test
 *     accepted them.
 *   - conversely it is WIDER on the destination: any CDT in the CDL now
 *     receives, not just the three Con.IDs the macros named. The lab captures
 *     address OVMX at slots 1, 2, 7, 8, 10 and 11.
 *
 * `msg` / `msglen` are the p. 4-15 SYSAP payload (SCA content [58:], see
 * scs_rx.h). The CM parsers read absolute frame offsets, so they take the frame
 * from scsd_rx_current instead -- see the DESIGN CHOICE note at the delivery
 * site.
 */
static void scsd_sysap_msg_input(struct scs_cdt *cdt, const void *msg, size_t msglen,
                                 void *ctx)
{
    struct peer_state *ps = (struct peer_state *)ctx;
    struct scsd_rx *rx = scsd_rx_current.rx;
    const uint8_t *buf = scsd_rx_current.frame;
    ssize_t n = scsd_rx_current.len;

    (void)msg;
    (void)msglen;

    sysap_msg_input_calls++;

    if (cdt == NULL || ps == NULL || rx == NULL || buf == NULL) {
        return;
    }
    {
        struct scs_member_view mv;
        int cm_parsed = (scs_member_parse(buf, (size_t)n, &mv) == 0);
        /* vms-7c0: WAS `cm_parsed && (mv.remote_conid == OVMX_JOINER_CONID ||
         * mv.local_conid == OVMX_JOINER_CONID)`. The CDL has already decided
         * which connection this frame belongs to. */
        int cm_on_joiner_vc = (cdt == ps->cdt_joiner);
        /* vms-760: a 190-byte CM message is identified by its LENGTH CLASS and
         * msgtype, not by an opcode table -- the coordinator sends some of them
         * with msgtype 0x5b even on a long-established VC (d94-e3.pcap frame
         * 1212), and gating on 0x4b alone silently DISCARDED them.
         * vms-2f3: ...and the retransmit form 0x7b, which is what made the
         * rejoin asymmetric: a fresh identity never provokes a retransmission,
         * so it never meets 0x7b, while a returning one is deaf to every
         * retransmission from the first onward. OVMX_CM_NO_RETX=1 restores the
         * old deaf behaviour so that failure stays reproducible.
         * (The Con.ID half of the old predicate is GONE -- see the header.) */
        if (cm_parsed &&
            (mv.msgtype == SCS_MEMBER_MSGTYPE ||
             mv.msgtype == SCS_MEMBER_MSGTYPE_ALT ||
             (mv.msgtype == SCS_MEMBER_MSGTYPE_RETX &&
              getenv("OVMX_CM_NO_RETX") == NULL))) {
            if (ps->connected || (cm_on_joiner_vc && ps->joiner_connected)) {
                sysap_cm_messages++;
                /* Track the member's SYSAP send-msg# high-water (our ack
                 * target). Only category-0x01 config messages carry the
                 * membership dialogue; DLM (cat 0x02) rides here later. */
                if (mv.sysap_send_msg > ps->sysap_recv) {
                    ps->sysap_recv = mv.sysap_send_msg;
                    /* vms-584: remember what advanced it, so an ack can say
                     * which frame it names and how stale that frame was. */
                    ps->cm_recv_advanced_ms = monotonic_ms();
                    ps->cm_last_recv_cat = mv.category;
                    ps->cm_last_recv_op = mv.opcode;
                }

                /* vms-7a9: consume the peer's advertised VOTES from its
                 * cat-0x01 op-0x01 cluster-parameters message (grounded LE u16
                 * at VC body[22:24], spec sec 4j) and fold it into the quorum
                 * model, keyed by the peer's stable VMS$VAXcluster Con.ID. */
                if (mv.has_votes) {
                    cm_quorum_note_peer_votes(ps->remote_conid, mv.votes);
                }

                /* vms-d94 (FORMATION): the burst rides OUR joiner connection,
                 * not the member-opened one.
                 *
                 * vms-760 server-first (OVMX_PURE_SERVER, ESTABLISHED join): the
                 * MEMBER sends its config FIRST on the VC it opened; OVMX answers
                 * with ITS add-member burst on receipt (af2 143.759: member
                 * config ss=12 -> joiner config ss=12). Fire once, on the first
                 * member SYSAP config frame (not a commit/lock txn). */
                if ((getenv("OVMX_PURE_SERVER") != NULL ||
                     cm_rejoin_target_mode()) && !ps->cm_config_sent &&
                    !mv.is_member_txn) {
                    int c = cm_send_config_burst(rx->sock, (int)rx->ifindex, ps,
                                                 rx->our_hw_mac, rx->our_src_logical,
                                                 PS_LOCAL_CONID(ps), ps->remote_conid);
                    rx->cm_config_frames += c;
                    log_ts(stdout);
                    printf(" SCSD-I-CMCONFIG, answered member config with add-member"
                           " burst (%d frames) on the member VC local=0x%08X"
                           " remote=0x%08X (server-first)\n",
                           c, (unsigned)PS_LOCAL_CONID(ps), ps->remote_conid);
                    fflush(stdout);

                    /* THE IMMEDIATE DISK-DISCOVERY TRIGGER STOOD HERE AND IS
                     * DELETED (vms-096). vms-760's intent -- OVMX has answered
                     * VAX1's config, so open OUR OWN SCS$DIRECTORY connection
                     * and run MSCP disk discovery -- is unchanged and still
                     * happens; only this entry point is gone.
                     *
                     * IT WAS DEAD, and the argument is one line long: its guard
                     * opened with `ps->psc_credit_done &&`, and the flag's only
                     * writer was the unreachable `cm_op == 6` block above, so
                     * the guard was false for the entire life of every process
                     * this branch has ever run. Deleting the writer without
                     * deleting this would leave a trigger that reads as live
                     * and is not. */
                }

                /* vms-760: ACKNOWLEDGE the coordinator's category-0x01
                 * opcode-0x06 burst with a category-0x04 ack.
                 *
                 * This is the LAST step of the add-member transaction and the
                 * one OVMX was missing. GROUNDED, vax3-2to3 reference: after the
                 * commit (op 0x03) and the lock rebuilds (op 0x05), the
                 * coordinator sends a run of op 0x06 messages (frames 303-312,
                 * sm=9..18) and the joiner answers with cat-0x04 acks naming a
                 * rising watermark (frames 313/314/315: am=11/14/17). Observed
                 * live in d94-e1.pcap: VAX3 sent 14 op-0x06 frames (434-447) and
                 * OVMX answered NONE, so the transaction never completed and the
                 * CSB stayed 'open'. op 0x06 carries txn=0 -- it is NOT a
                 * token-correlated request, so it must NOT get a 0x81 echo. */
                /* vms-760: what distinguishes the two kinds of coordinator
                 * message is the CORRELATION TOKEN, not the opcode.
                 * GROUNDED over the reference's whole admission tail:
                 *   txn != 0  (0x03 txn=8, 0x05 txn=9..11, 0x09 txn=8)
                 *             -> token-correlated request, answer 0x81 echo
                 *   txn == 0  (0x06, 0x0a, 0x0c)
                 *             -> not correlated, answer a cat-0x04 ack
                 * Keying on an opcode allowlist instead is what left op 0x09
                 * unanswered and stalled the transaction (d94-e2.pcap: VAX3
                 * sent 293 op-0x06 then op 0x09 txn=10 and everything
                 * stopped). This rule needs no list and covers 0x0a/0x0c too. */
                /* The membership dialogue spans more than one category: the
                 * coordinator finishes it with a category-0x06 request that
                 * takes the same echo transform (ref frames 671 -> 673:
                 * cat 0x06 op 0x00 txn=8 cksum=0x9a2c -> cat 0x86, same token).
                 * Category 0x02 (the distributed lock manager) IS included, for
                 * one specific reason: during the join the coordinator replays
                 * the lock-resource database at the joiner as token-correlated
                 * cat-0x02 op-0x0d transactions, INTERLEAVED with the barrier,
                 * and it will not release the next barrier step until they are
                 * answered. Observed exactly: five cat-0x02 requests arrived
                 * unanswered and the barrier froze at step 5 (d94-e12).
                 * Answering these is not a lie about lock state: a joining node
                 * holds no locks, so acknowledging a rebuild record is simply
                 * what a joiner has to say. Real lock semantics -- granting,
                 * denying, blocking, remastering -- belong to the lock manager
                 * and are NOT implemented here; this path must be revisited when
                 * OVMX actually holds locks. */
                int cm_req = !mv.is_response &&
                             (mv.category == SCS_MEMBER_CAT_CONFIG ||
                              mv.category == SCS_MEMBER_CAT_MEMBERSHIP ||
                              mv.category == SCS_MEMBER_CAT_DLM);
                int cm_token_req = cm_req && mv.txn != 0;
                int cm_plain_req = cm_req && mv.txn == 0;

                /* vms-760: TRACE EVERY inbound CM message. This exists because
                 * a run stalled at barrier step 1 and the logs could not say
                 * whether the coordinator's op-0x0c release had arrived and
                 * been dropped, or had never been sent -- two completely
                 * different bugs that looked identical from here. We logged
                 * only what we ACTED on, so anything we silently ignored was
                 * invisible exactly when it mattered most.
                 * Set OVMX_CM_QUIET to suppress (the DLM storm is ~216 lines). */
                if (getenv("OVMX_CM_QUIET") == NULL) {
                    log_ts(stdout);
                    printf(" SCSD-T-CMIN, cat 0x%02x op 0x%02x txn=0x%04x"
                           " csum=0x%04x smsg=%u amsg=%u%s%s\n",
                           mv.category, mv.opcode, mv.txn, mv.checksum,
                           mv.sysap_send_msg, mv.sysap_ack_msg,
                           mv.is_response ? " (RESPONSE)" : "",
                           /* vms-2f3: mark the retransmit form. A peer only
                            * sends it when it did not get an answer, so a
                            * run full of these is a run we were deaf in. */
                           mv.msgtype == SCS_MEMBER_MSGTYPE_RETX
                               ? " (RETRANSMIT 0x7b)" : "");
                    fflush(stdout);
                }

                /* vms-197 (spec §4(O.38)): RECOGNISE OUR OWN ADMISSION from the
                 * AUTHENTIC wire signal -- the coordinator's cat-0x01 op 0x06
                 * MEMBERSHIP burst (its post-commit membership publication). This
                 * is a PURE READ of a frame OVMX already receives, acks and traces
                 * (see the ack-cadence block below): nothing here changes the
                 * wire, the CM state machine, or any emitted frame. It only
                 * corrects what OVMX CONCLUDES about its own admission.
                 *
                 * WHY THIS AND NOT THE BARRIER: the pre-vms-197 self-report
                 * (SCSD-I-XITDONE) fired ONLY when OVMX completed all 12 op-0x0c
                 * barrier steps. That barrier RACES the coordinator's fan-out and
                 * can stall part-way even on a genuine admission -- §4(O.38)'s
                 * five-return oracle sweep caught return 3 as a member-oracle WIN
                 * (CLUSTER_NODES 2->3 on BOTH members) that completed only 4/12
                 * barrier steps and therefore self-reported XITDONE=0: a PHANTOM
                 * LOSE that dragged §4(O.33)-(O.37) in circles. The op 0x06
                 * membership burst is 1:1 with the member oracle across that same
                 * sweep (254 frames on every WIN incl. the phantom r3; 0 on the
                 * genuine LOSE r4), because the coordinator only publishes the new
                 * membership once the transition COMMITS cluster-wide (Rule of
                 * Total Connectivity, Davis p.7-39) -- exactly the condition the
                 * oracle measures. So XITDONE is emitted from THIS, latched once. */
                if (cm_plain_req && mv.category == SCS_MEMBER_CAT_CONFIG &&
                    mv.opcode == SCS_MEMBER_OP_MEMBERSHIP) {
                    ps->membership_bursts++;
                    if (!ovmx_cluster.membership_committed) {
                        ovmx_cluster.membership_committed = 1;
                        log_ts(stdout);
                        printf(" SCSD-I-XITDONE, cluster membership COMMITTED --"
                               " received the coordinator's op 0x06 MEMBERSHIP burst"
                               " (post-commit membership publication) from"
                               " %02x:%02x:%02x:%02x:%02x:%02x. This is the"
                               " AUTHENTIC admission signal (1:1 with the member"
                               " oracle F$GETSYI(\"CLUSTER_NODES\") 2->3, spec"
                               " §4(O.38)): the coordinator publishes it only once"
                               " the transition commits cluster-wide, so its receipt"
                               " means OVMX is now an admitted cluster member --"
                               " whether or not OVMX's own op-0x0c barrier"
                               " (SCSD-I-XITBARRIER) has finished\n",
                               ps_port_addr(ps)[0], ps_port_addr(ps)[1],
                               ps_port_addr(ps)[2], ps_port_addr(ps)[3],
                               ps_port_addr(ps)[4], ps_port_addr(ps)[5]);
                        fflush(stdout);
                    }
                }

                /* vms-c21 (spec §4(O.33)): the cat 0x01 op 0x04 role 0x50 CM
                 * message. HISTORY: vms-2f3 read this as "THE TRANSITION ABORT
                 * -- a DECISION, we are being refused". §4(O.33) REFUTES that
                 * reading against the authoritative membership oracle (each
                 * member's F$GETSYI("CLUSTER_NODES")): this exact message is
                 * present in the SUCCESSFUL fresh first-join too (with the VAX
                 * reaching CLUSTER_NODES=3) and ABSENT from the reproducible
                 * failure, so it does NOT separate admit from non-admit and is
                 * NOT a returning-identity refusal -- it is a normal per-member
                 * CM barrier/transition message. OVMX cannot classify the
                 * coordinator's decision from its own side (§4(O.32)); log it as
                 * an OBSERVATION only and assert no membership conclusion.
                 *
                 * Detection UNCHANGED (byte-for-byte); only the logged text is
                 * made honest. Matched on the ROLE SLOT, never the opcode alone
                 * -- op 0x04 role 0x00 is a different SYSAP's opcode 4 and
                 * appears in SUCCESSFUL joins (handoff sec 3.4). Not answered
                 * (txn=0, spec sec 4(r)); real participants send nothing back. */
                if (cm_req && mv.category == SCS_MEMBER_CAT_CONFIG &&
                    mv.opcode == SCS_MEMBER_OP_ABORT && (size_t)n >= 92 &&
                    buf[72 + SCS_MEMBER_ROLE_BODYOFF] == SCS_MEMBER_ROLE_ABORT) {
                    uint8_t cls = buf[72 + SCS_MEMBER_CLASS_BODYOFF];
                    rx->cm_abort_seen++;
                    log_ts(stdout);
                    printf(" SCSD-I-CMOP04, node %u sent cat 0x01 op 0x04 role"
                           " 0x50 (class 0x%02x) -- a per-member CM barrier/"
                           "transition message. OBSERVED ONLY: this message"
                           " occurs on BOTH successful and failed joins (spec"
                           " 4(O.33)); it is NOT an abort/refusal DECISION and"
                           " does NOT mean the coordinator declined this node."
                           " NON-AUTHORITATIVE -- the authoritative membership"
                           " signal is each MEMBER's F$GETSYI(\"CLUSTER_NODES\")"
                           " / CDT layer, not this frame (spec 4(O.32)/4(O.33))\n",
                           (unsigned)(peer_node_number(ps) & 0x03ff),
                           (unsigned)cls);
                    fflush(stdout);
                }

                /* Remember which VC this dialogue rides so the poll-loop
                 * flush can answer on the same one. */
                if (cm_on_joiner_vc) {
                    ps->cm_local_conid = PS_JOINER_CONID(ps);
                    ps->cm_remote_conid = ps->joiner_remote_conid;
                } else {
                    ps->cm_local_conid = PS_LOCAL_CONID(ps);
                    ps->cm_remote_conid = ps->remote_conid;
                }

                /* vms-e81: LEARN the cluster-wide facts from a MEMBER's own
                 * op 0x01. body[12]==0x21 is the member form (joiner: 0x00),
                 * and only a member's copy is worth anything -- a joiner's
                 * carries zeros. We COPY formation and last-transition
                 * verbatim and never compute them: they are facts about the
                 * cluster, not about us, and inventing either would be the
                 * Rule 10 failure this whole item keeps circling. The member
                 * count comes from the same frame, so all three always agree
                 * with each other and with whoever we heard them from. */
                if (cm_req && mv.category == SCS_MEMBER_CAT_CONFIG &&
                    mv.opcode == SCS_MEMBER_OP_PARAMS && (size_t)n >= 204 &&
                    buf[72 + 12] == 0x21) {
                    uint64_t formed = 0, lastx = 0;
                    for (int k = 7; k >= 0; k--) {
                        formed = (formed << 8) | buf[72 + 28 + k];
                        lastx  = (lastx  << 8) | buf[72 + 36 + k];
                    }
                    uint16_t mc = (uint16_t)buf[72 + 18] |
                                  ((uint16_t)buf[72 + 19] << 8);
                    /* vms-2f3: IDENTIFY THE FOUNDING NODE while we have its
                     * own frame in hand. body[64:72] is the sender's own
                     * admission time (SDA CSB `Ref. time`) and body[28:36]
                     * is the cluster's founding time (SDA CLUB `Founding
                     * Time`). They are equal for exactly one member -- the
                     * node that founded the cluster, whose admission IS the
                     * founding. Its SCSSYSTEMID is the LE16 in its own
                     * source-logical address aa:00:04:00:<sysid> at abs 24.
                     * d94-r1B: 3 members, 1 match (VAX1/1025); SDA on VAX1
                     * and on VAX3 both report Found Node SYSID 0x0401. */
                    uint64_t sender_admission = 0;
                    for (int k = 7; k >= 0; k--) {
                        sender_admission = (sender_admission << 8) | buf[72 + 64 + k];
                    }
                    if (formed != 0 && sender_admission == formed) {
                        uint16_t fs = (uint16_t)buf[24 + 4] |
                                      ((uint16_t)buf[24 + 5] << 8);
                        if (fs != 0 && ovmx_cluster.founding_sysid != fs) {
                            ovmx_cluster.founding_sysid = fs;
                            log_ts(stdout);
                            printf(" SCSD-I-CLUFOUND, node %u FOUNDED this"
                                   " cluster: its own admission time equals"
                                   " the cluster founding time"
                                   " (0x%016llx) -- SDA calls this the"
                                   " CLUB's Found Node SYSID\n",
                                   (unsigned)fs,
                                   (unsigned long long)formed);
                            fflush(stdout);
                        }
                    }
                    /* vms-584: this copy BOOTSTRAPS us -- it is how we learn
                     * the cluster at all before we have witnessed any
                     * transition. But it must never walk us BACKWARDS.
                     *
                     * A member's op 0x01 is a point-in-time reply, and it is
                     * sent to a newcomer BEFORE that newcomer is counted:
                     * VAX1 answered OVMX 6.7 s early and VAX3 4.1 s early,
                     * and never advertised 4 at all. So a copy arriving
                     * after we have re-learned from a completed transition
                     * can easily carry an OLDER view than we already hold.
                     *
                     * last_transition is monotonic in time, which makes the
                     * ordering test exact: accept the copy only if its
                     * transition is at least as recent as ours. `formed`
                     * never changes and is always safe to take. */
                    if (lastx < ovmx_cluster.last_transition) {
                        log_ts(stdout);
                        printf(" SCSD-I-CLUSTATE, IGNORED a peer's op 0x01"
                               " (members=%u last_transition=0x%016llx):"
                               " older than what we hold (0x%016llx) --"
                               " a member answers a newcomer before that"
                               " newcomer is counted\n",
                               (unsigned)mc, (unsigned long long)lastx,
                               (unsigned long long)ovmx_cluster.last_transition);
                        fflush(stdout);
                        ovmx_cluster.formed = formed;
                        ovmx_cluster.known = 1;
                    } else {
                        if (!ovmx_cluster.known || ovmx_cluster.member_count != mc ||
                            ovmx_cluster.last_transition != lastx) {
                            log_ts(stdout);
                            printf(" SCSD-I-CLUSTATE, learned member-form cluster state"
                                   " from a peer: members=%u formed=0x%016llx"
                                   " last_transition=0x%016llx\n",
                                   (unsigned)mc, (unsigned long long)formed,
                                   (unsigned long long)lastx);
                            fflush(stdout);
                        }
                        ovmx_cluster.known = 1;
                        ovmx_cluster.member_count = mc;
                        ovmx_cluster.formed = formed;
                        ovmx_cluster.last_transition = lastx;
                    }
                }

                /* vms-e81 ROOT CAUSE: A SITTING MEMBER MUST RECIPROCATE A
                 * NEWCOMER'S CONFIG, IMMEDIATELY.
                 *
                 * This is what stopped VAX3 joining while OVMX sat there as a
                 * member, and the capture contains its own control. In the
                 * pure-VMS reference a newcomer sends each member it has a VC
                 * with `op 0x14` then `op 0x01`, and the member answers with
                 * its OWN `op 0x14` + `op 0x01` within ONE MILLISECOND (VAX1
                 * +0.9 ms, VAX2 +0.3 ms). The newcomer then emits its
                 * `cat 0x01 op 0x02` add-member request ~110 ms after the LAST
                 * member reciprocates -- and its `amsg` literally acknowledges
                 * that reciprocal `op 0x01`. The trigger is data-driven, not a
                 * timer.
                 *
                 * OVMX never reciprocated. So VAX3 sent us its config, waited,
                 * and never asked ANYONE for admission: zero `op 0x02` from it
                 * for 678 s, and zero `op 0x12` naming it. It was not ignoring
                 * us and it was not refusing us -- it was WAITING ON US, again.
                 *
                 * The proof is a natural experiment inside the same capture.
                 * OVMX's process exited at +702 s; VAX1/VAX2 ran the removal
                 * transition at +730; VAX1 and VAX2 each re-pushed their
                 * `op 0x01` to VAX3 -- and at +733.5, THREE SECONDS after OVMX
                 * left the membership and with nothing else changed, VAX3 sent
                 * its `op 0x02` and joined normally. The only variable was us.
                 *
                 * Scoped to `appeared_after_join`, which is exactly the
                 * grounded configuration: a node we met only AFTER our own
                 * admission is a newcomer joining a cluster we are already in.
                 * Our own join path is untouched -- there we are the joiner and
                 * the burst rides the joiner-initiated VC. */
                if (cm_req && mv.category == SCS_MEMBER_CAT_CONFIG &&
                    mv.opcode == SCS_MEMBER_OP_MODEL &&
                    !cm_on_joiner_vc && ps->appeared_after_join &&
                    !ps->cfg_sent && ps->connected) {
                    int c = cm_send_config_burst(rx->sock, (int)rx->ifindex, ps,
                                                 rx->our_hw_mac, rx->our_src_logical,
                                                 PS_LOCAL_CONID(ps),
                                                 ps->remote_conid);
                    rx->cm_config_frames += c;
                    log_ts(stdout);
                    printf(" SCSD-I-CMRECIP, newcomer sent its config --"
                           " reciprocated with our own op 0x14 + op 0x01"
                           " (%d frames) on the VC it opened to us."
                           " The reference does this within 1 ms; a newcomer"
                           " that does not get it never asks to join\n", c);
                    fflush(stdout);
                }

                /* Ack cadence: batch, and let the poll loop flush the tail. */
                /* Only the op-0x06 burst is acknowledged. op 0x0a and op 0x0c
                 * also carry txn=0 but are NOTIFICATIONS: no 0x8a/0x8c response
                 * exists in any capture, and no cat-0x04 ack is attributable to
                 * them. Consume them, advance our ack-msg#, emit nothing. */
                if (cm_plain_req && mv.opcode == SCS_MEMBER_OP_MEMBERSHIP &&
                    (uint16_t)(ps->sysap_recv - ps->sysap_acked) >= SCS_CM_ACK_EVERY &&
                    !rejoin_hold_standalone_ack(ps) && /* vms-71d */
                    cm_send_ack(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac, rx->our_src_logical)) {
                    log_ts(stdout);
                    printf(" SCSD-I-CMACK, cat-0x04 ack (ack_msg=%u)\n",
                           ps->sysap_acked);
                    fflush(stdout);
                }

                /* vms-760: the cluster-wide state-transition BARRIER (spec 4p).
                 *   op 0x09 (tag 0x0240) -> answer, and latch the epoch
                 *   op 0x0a (tag 0x0260) -> no answer; start the barrier at N=1
                 *   op 0x0c (step N)     -> no answer; advance to N+1, or finish
                 * The joiner is a REQUIRED participant: the coordinator gates every
                 * member at step N until all of them have sent their 0x0b. */
                /* vms-e4b: ALL THREE transition-opens fill the same role slot
                 * (body[16] = 0x40) and carry the epoch in the same place --
                 * op 0x09 opens a class-0x02 add, op 0x08 a class-0x03 removal
                 * of a failed node, op 0x0d a class-0x04 self-departure. We
                 * previously latched the epoch only from op 0x09, so during
                 * anyone else's removal or departure we carried a stale epoch.
                 *
                 * The opcode is still the key (see cm_response_shape); the role
                 * slot is only a cross-check, and a mismatch is logged rather
                 * than acted on -- we do not want to start trusting body[16] as
                 * an identifier by the back door. */
                if (cm_req && (size_t)n >= 92 &&
                    /* CATEGORY FIRST, ALWAYS. op 0x0d is the class-0x04
                     * transition-open in category 0x01 and the DLM lock-resource
                     * rebuild record in category 0x02, and a join carries 216 of
                     * the latter. Matching on the opcode alone latched a lock
                     * record's bytes as the transition epoch and class -- the
                     * barrier then carried epoch 0x00030001 into step 6 and
                     * stalled. Caught live by the role cross-check below firing
                     * 223 times, which is precisely why that cross-check logs
                     * instead of gating. */
                    mv.category == SCS_MEMBER_CAT_CONFIG &&
                    (mv.opcode == SCS_MEMBER_OP_XITION ||
                     mv.opcode == SCS_MEMBER_OP_08 ||
                     mv.opcode == SCS_MEMBER_OP_DEPART)) {
                    uint8_t role = buf[72 + SCS_MEMBER_ROLE_BODYOFF];
                    ps->barrier_epoch = (uint32_t)buf[84] |
                                        ((uint32_t)buf[85] << 8) |
                                        ((uint32_t)buf[86] << 16) |
                                        ((uint32_t)buf[87] << 24);
                    ps->xition_class = buf[72 + SCS_MEMBER_CLASS_BODYOFF];
                    /* vms-584: the open carries the post-transition cluster
                     * facts. Latch them now, apply them when the transition
                     * is real (see ovmx_cluster_relearn). */
                    /* The enclosing guard is n >= 92, which is enough for the
                     * epoch but NOT for these: body[40:48] needs n >= 120 and
                     * body[55] needs n >= 128. Reading them under the outer
                     * guard alone would take whatever was left in the buffer
                     * from a previous frame and turn it into an advertised
                     * member count. Check the bytes we actually touch. */
                    if ((size_t)n >= 72 + 56) {
                        uint64_t xtime = 0;
                        for (int k = 7; k >= 0; k--) {
                            xtime = (xtime << 8) | buf[72 + 40 + k];
                        }
                        uint8_t bm = buf[72 + 55];
                        uint16_t pop = 0;
                        for (int k = 0; k < 8; k++) {
                            if (bm & (1u << k)) pop++;
                        }
                        ps->pending_members = pop;
                        ps->pending_last_transition = xtime;
                        ps->pending_state = 1;
                        /* Class 0x04 runs NO barrier -- there is no later
                         * completion to wait for, so its facts are final at
                         * the open. Classes 0x02/0x03 wait for our own
                         * barrier so we never advertise a transition that
                         * was proposed and then aborted. */
                        if (ps->xition_class == SCS_MEMBER_CLASS_DEPART) {
                            ovmx_cluster_relearn(pop, xtime,
                                                 "class-0x04 departure, no barrier follows");
                            ps->pending_state = 0;
                            /* DLM rung H10a (rd vms-2bf): a GRACEFUL class-0x04
                             * self-departure is the departure INGRESS. Tell THIS
                             * node's executive to drop the departed peer's CSID
                             * from the LIVE DLM directory membership (LOCAL ioctl,
                             * not a new cross-node send). A no-op unless armed for
                             * the H10 harness (OVMX_DLM_H10). */
                            scsd_dlm_h10_depart_ingress(ps);
                            /* DLM rung H10b (rd vms-dca9): if THIS node holds a
                             * cross-node lock on RES_C via the departing peer,
                             * re-register it on the resource's NEW directory
                             * master. A no-op on any node that never
                             * established the hold (see the function). */
                            scsd_dlm_h10b_rebuild_on_depart(ps);
                        }
                    }
                    /* The role slot is a CROSS-CHECK, never a gate. Gating the
                     * epoch latch on it would let one unexpected byte silently
                     * stop us tracking the transition -- the failure mode this
                     * whole item exists to remove. Log the mismatch instead. */
                    if (role != SCS_MEMBER_ROLE_XITION) {
                        log_ts(stdout);
                        printf(" SCSD-W-XITROLE, op 0x%02x carries role slot 0x%02x,"
                               " expected 0x%02x -- latching anyway, but the role"
                               " model may be wrong\n",
                               mv.opcode, role, (unsigned)SCS_MEMBER_ROLE_XITION);
                        fflush(stdout);
                    }
                    log_ts(stdout);
                    printf(" SCSD-I-XITION, state transition opened by op 0x%02x"
                           " (epoch=0x%08X class=0x%02x %s)\n",
                           mv.opcode, (unsigned)ps->barrier_epoch,
                           ps->xition_class,
                           ps->xition_class == SCS_MEMBER_CLASS_ADD ? "ADD" :
                           ps->xition_class == SCS_MEMBER_CLASS_REMOVE ? "REMOVE-FAILED" :
                           ps->xition_class == SCS_MEMBER_CLASS_DEPART ? "SELF-DEPARTURE" :
                           "UNKNOWN-CLASS");
                    fflush(stdout);
                }
                /* vms-e81 (T1.1): the barrier must arm for EVERY transition,
                 * not just our own join.
                 *
                 * This used to require !ps->barrier_done, and barrier_done
                 * latches when OUR join completes step 12 -- so once OVMX
                 * became a member it could NEVER arm the barrier again.
                 * Every later transition (any other node joining or leaving)
                 * would have found OVMX silent, and spec 4(p) is explicit
                 * about what that does: the coordinator gates every step on
                 * EVERY member answering, so a silent member strands the
                 * transition, it times out, '%CNXMAN, aborting VAXcluster
                 * state transition' is logged, and HEALTHY MEMBERS ARE
                 * DROPPED. OVMX would have been a hazard to any cluster it
                 * sat in, triggered by a join it had nothing to do with.
                 *
                 * barrier_done is now a record that our OWN admission
                 * finished, not a gate on future participation. Only
                 * barrier_step (a barrier already running) suppresses a
                 * re-arm. */
                /* Category-qualified for the same reason as the open above:
                 * cm_req spans categories 0x01, 0x02 and 0x06, so an opcode
                 * alone is not an identifier. */
                if (cm_req && mv.category == SCS_MEMBER_CAT_CONFIG &&
                    mv.opcode == SCS_MEMBER_OP_XITION_GO &&
                    (size_t)n >= 90 && !ps->barrier_step) {
                    uint16_t tag = (uint16_t)buf[88] | ((uint16_t)buf[89] << 8);
                    /* vms-e4b: the tag is (class << 8) | role, and the barrier
                     * arms for TWO of the three classes.
                     *
                     *   0x0260 class-0x02 ADD    -> 12-step barrier. Armed.
                     *   0x0360 class-0x03 REMOVE -> 12-step barrier. Armed.
                     *                               THIS WAS THE HOLE. A node
                     *                               failing is not exotic; it is
                     *                               the single most likely event
                     *                               to happen around a sitting
                     *                               member, and we answered it
                     *                               with silence -- which spec
                     *                               4(p) says strands the
                     *                               transition and drops healthy
                     *                               members.
                     *   0x0460 class-0x04 DEPART -> NO barrier exists. An
                     *                               0x81/0x0b carrying class
                     *                               0x04 does not occur in any
                     *                               capture; the dialogue is
                     *                               0x12, 0x03, 0x0d, 0x0a and
                     *                               then nothing. Arming here
                     *                               would make us send a step-1
                     *                               request nobody ever asked
                     *                               for -- inventing traffic for
                     *                               a condition VMS does not
                     *                               face. Stay quiet.
                     *
                     * Any other tag stays unhandled AND LOUD. It is not silence
                     * we have chosen, it is a gap we have not grounded, and the
                     * log line is what turns it into work. */
                    if (tag == SCS_MEMBER_BARRIER_TAG ||
                        tag == SCS_MEMBER_BARRIER_TAG_REM) {
                        /* DO NOT answer from here -- see barrier_go_pending.
                         * Replying at receive-path speed (~20 us) beats the
                         * coordinator's own 0x0a fan-out and gets our step
                         * uncounted. Defer to the poll loop. */
                        ps->barrier_step = 1;
                        ps->barrier_go_ms = monotonic_ms();
                        ps->barrier_go_pending = 1;
                        log_ts(stdout);
                        printf(" SCSD-I-XITGO, barrier GO received -- deferring"
                               " step 1 by %d ms so it lands after the"
                               " coordinator's fan-out\n",
                               (int)JOIN_BARRIER_GO_DELAY_MS);
                        fflush(stdout);
                        /* Sleep here rather than leaving it to the poll loop:
                         * the receive loop only wakes on traffic or a 1 s
                         * SO_RCVTIMEO, so the poll path would make the gap
                         * anywhere from 3 ms to 1 s. We have nothing else to
                         * do during the GO phase, and the whole point is to
                         * control this interval precisely. The poll-loop
                         * emitter below stays as a belt-and-braces fallback. */
                        struct timespec gowait;
                        gowait.tv_sec = 0;
                        gowait.tv_nsec = (long)JOIN_BARRIER_GO_DELAY_MS * 1000000L;
                        nanosleep(&gowait, NULL);
                        ps->barrier_go_pending = 0;
                        cm_send_barrier_step(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                             rx->our_src_logical, ps->barrier_step);
                    } else if (tag == (uint16_t)((SCS_MEMBER_CLASS_DEPART << 8) |
                                                 SCS_MEMBER_ROLE_GO)) {
                        log_ts(stdout);
                        printf(" SCSD-I-XITGO, op 0x0a tag 0x%04x is a class-0x04"
                               " SELF-DEPARTURE -- no barrier follows one, so we"
                               " correctly send nothing\n", tag);
                        fflush(stdout);
                    } else {
                        log_ts(stdout);
                        printf(" SCSD-W-XITGOUNGROUNDED, op 0x0a with tag 0x%04x"
                               " (role 0x%02x class 0x%02x) -- we have never"
                               " observed this class; NOT arming the barrier."
                               " Ground it before answering\n",
                               tag, (unsigned)(tag & 0xff), (unsigned)(tag >> 8));
                        fflush(stdout);
                    }
                }
                if (cm_req && mv.category == SCS_MEMBER_CAT_CONFIG &&
                    mv.opcode == SCS_MEMBER_OP_BARRIER_REL &&
                    (size_t)n >= 92) {
                    uint32_t rel = (uint32_t)buf[88] | ((uint32_t)buf[89] << 8) |
                                   ((uint32_t)buf[90] << 16) | ((uint32_t)buf[91] << 24);
                    /* vms-760: a release that does NOT match our current step
                     * used to be dropped in total silence -- so "the release
                     * never came" and "the release came and we ignored it"
                     * were indistinguishable in the logs. Say which. */
                    if (!ps->barrier_step || (int)rel != ps->barrier_step) {
                        log_ts(stdout);
                        printf(" SCSD-W-XITREL, IGNORED op 0x0c release for step"
                               " %u -- our barrier_step is %d%s\n",
                               (unsigned)rel, ps->barrier_step,
                               ps->barrier_step ? "" : " (barrier not running)");
                        fflush(stdout);
                    }
                    if (ps->barrier_step && (int)rel == ps->barrier_step) {
                        if (ps->barrier_step >= SCS_MEMBER_BARRIER_STEPS) {
                            ps->barrier_done = 1;
                            ps->barrier_step = 0;
                            /* vms-e81: our OWN admission time, stamped when
                             * our barrier completes. The joiner template
                             * carries the sentinel 00804a3f0e579f00, which
                             * decodes as exactly 2001-01-01 00:00:00 -- a
                             * node advertising that while claiming to be a
                             * member says it was admitted 25 years before
                             * the cluster it is in was formed. */
                            if (!ovmx_cluster.admitted) {
                                ovmx_cluster.admitted = 1;
                                ovmx_cluster.own_admission =
                                    scs_member_vms_time_now();
                                /* vms-2f3: this is the moment a rebooted VAX
                                 * would have something to remember. Record
                                 * it now, while it is a fact and not a
                                 * prediction. */
                                prior_admission_save();
                            }
                            ps->barrier_count++;
                            /* vms-584: the transition this barrier belonged
                             * to is now REAL, so adopt the cluster facts its
                             * open carried. Doing it here rather than at the
                             * open means a proposal that never completes
                             * never becomes something we advertise. */
                            if (ps->pending_state) {
                                ovmx_cluster_relearn(ps->pending_members,
                                                     ps->pending_last_transition,
                                                     "our barrier completed");
                                ps->pending_state = 0;
                            }
                            log_ts(stdout);
                            /* vms-197 (spec §4(O.38)): this records OVMX completing
                             * its OWN op-0x0c barrier participation -- a real event,
                             * but NOT the admission verdict. It was formerly emitted
                             * as SCSD-I-XITDONE and misread as "OVMX is admitted",
                             * yet it RACES the coordinator's fan-out and can stall
                             * part-way on a genuine member-oracle WIN (§4(O.38) r3:
                             * 4/12 steps -> phantom LOSE). The authentic admission
                             * self-report (SCSD-I-XITDONE) is now driven by the
                             * received op 0x06 MEMBERSHIP burst above; this line is
                             * renamed so it no longer masquerades as that verdict or
                             * double-counts it. */
                            printf(" SCSD-I-XITBARRIER, own barrier COMPLETE"
                                   " (all %d op-0x0c steps released; this is"
                                   " transition #%u for this peer -- #1 is our"
                                   " own admission, later ones are transitions"
                                   " we participate in as a MEMBER). NOTE: barrier"
                                   " completion is NOT the admission verdict --"
                                   " the authentic admission self-report is the op"
                                   " 0x06 membership-burst line (spec §4(O.38))\n",
                                   SCS_MEMBER_BARRIER_STEPS,
                                   (unsigned)ps->barrier_count);
                            fflush(stdout);
                        } else {
                            ps->barrier_step++;
                            cm_send_barrier_step(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                                 rx->our_src_logical, ps->barrier_step);
                        }
                    }
                }

                /* Answer every token-correlated member-driven transaction by
                 * echoing its (txn,checksum) -- commit 0x03, lock rebuild
                 * 0x05, and 0x09 (spec sec 4j). */
                int cm_shape = cm_token_req
                    ? cm_response_shape(mv.category, mv.opcode)
                    : CM_RSP_NONE;
                if (cm_token_req && cm_shape == CM_RSP_NONE) {
                    /* vms-760: NOT grounded -- stay silent rather than invent a
                     * reply. Answering blindly here crashed VAX1 and VAX3. */
                    log_ts(stdout);
                    printf(" SCSD-W-CMUNGROUNDED, NO response sent to member"
                           " cat 0x%02x op 0x%02x txn=0x%04x -- this (cat,op)"
                           " pair is not grounded; ground it before answering\n",
                           mv.category, mv.opcode, mv.txn);
                    fflush(stdout);
                }
                if (cm_shape != CM_RSP_NONE) {
                    if (ps->sysap_send == 0) {
                        ps->sysap_send = 1;
                    }
                    struct scs_member_params mp;
                    memset(&mp, 0, sizeof(mp));
                    memcpy(mp.dst_mac, ps_port_addr(ps), 6);
                    memcpy(mp.src_mac, rx->our_hw_mac, 6);
                    memcpy(mp.src_logical, rx->our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
                    memcpy(mp.peer_logical, ps_sys_addr(ps), 6);
                    /* vms-760: answer on the SAME virtual circuit the request
                     * arrived on -- the joiner-opened VC in OVMX_JOIN_SEQ mode,
                     * the member-opened one otherwise. */
                    if (cm_on_joiner_vc) {
                        mp.remote_conid = ps->joiner_remote_conid;
                        mp.local_conid = PS_JOINER_CONID(ps);
                    } else {
                        mp.remote_conid = ps->remote_conid;
                        mp.local_conid = PS_LOCAL_CONID(ps);
                    }
                    mp.incarnation = ps->incarnation;
                    mp.recv_ack = ps->vc.seq.recv_seq;
                    mp.send_seq = scs_seq_advance(&ps->vc.seq);
                    mp.sysap_send_msg = ps->sysap_send++;
                    mp.sysap_ack_msg = mv.sysap_send_msg; /* ack this request */
                    /* The response SHAPE is per-category and comes from the
                     * grounded allowlist in cm_response_shape() -- never from a
                     * default. See that function for why. */
                    uint8_t rframe[SCS_MEMBER_FRAME_LEN];
                    int rc_build;
                    if (cm_shape == CM_RSP_TOKEN) {
                        rc_build = scs_member_build_token_response(
                            &mp, buf, (size_t)n, rframe);
                    } else if (cm_shape == CM_RSP_DLM) {
                        rc_build = scs_member_build_dlm_response(
                            &mp, buf, (size_t)n, rframe);
                    } else {
                        rc_build = scs_member_build_response(
                            &mp, buf, (size_t)n, rframe);
                    }
                    if (rc_build == 0 && mv.category == SCS_MEMBER_CAT_CONFIG &&
                        mv.opcode == SCS_MEMBER_OP_RELAY) {
                        /* op 0x12's response is not a pure echo. CORRECTED by
                         * vms-e81 against 6 request/response pairs in 4
                         * captures at three DIFFERENT epoch values:
                         *
                         *   body[16:18] = 0x0210   UNCONDITIONAL rewrite
                         *   body[20:24] = the request's body[12:16] -- a copy
                         *                 of the TRANSITION EPOCH
                         *
                         * The previous code wrote a post-transition MEMBER
                         * COUNT into body[20:24]. That was inferred from two
                         * specimens in which the epoch and the member count
                         * happened to coincide (3 and 4). Varying the epoch
                         * across captures separates them, and it is the epoch.
                         * A plausible reading that survives two samples and
                         * dies on six is exactly the trap this project keeps
                         * hitting.
                         *
                         * body[16:18] being a REWRITE was invisible in our own
                         * capture because the request already carried 0x0210;
                         * three other specimens carry 0x0410 in the request and
                         * 0x0210 in the response. */
                        /* vms-e4b REFINEMENT: body[17] is not the constant
                         * 0x02, it is the CLASS of the transition the
                         * responder is currently in. It read as a constant
                         * because every specimen we had was a class-0x02 ADD
                         * -- which is also the only class OVMX has ever been
                         * in, so this is a no-op on the proven join path and
                         * only differs once a removal or a departure happens
                         * around us. Default 0x02 when we have latched
                         * nothing, i.e. exactly the old behaviour. */
                        uint8_t *rb = rframe + 72;
                        const uint8_t *qb = buf + 72;
                        rb[16] = SCS_MEMBER_ROLE_RELAY;
                        rb[17] = ps->xition_class ? ps->xition_class
                                                  : SCS_MEMBER_CLASS_ADD;
                        rb[20] = qb[12];
                        rb[21] = qb[13];
                        rb[22] = qb[14];
                        rb[23] = qb[15];
                        /* body[55] stays echoed -- it sits inside the DLM
                         * record residue the relay carries. The generic
                         * transform no longer clears it for op 0x12 either
                         * (that rule is op-0x09-specific), but be explicit. */
                        rb[55] = qb[55];
                    }
                    if (rc_build == 0 &&
                        send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                                  "0x81 CM transaction response",
                                  rframe,
                                      sizeof(rframe)) > 0) {
                        ps->cm_responses++;
                        rx->cm_response_sent++;
                        log_ts(stdout);
                        printf(" SCSD-I-CMRESP, 0x81 response to member cat 0x%02x"
                               " op 0x%02x txn=0x%04x csum=0x%04x (%s)"
                               " send_msg=%u ack_msg=%u\n",
                               mv.category, mv.opcode, mv.txn, mv.checksum,
                               cm_shape == CM_RSP_TOKEN ? "token-only" : (cm_shape == CM_RSP_DLM ? "dlm-echo" : "echoed"),
                               mp.sysap_send_msg, mp.sysap_ack_msg);
                        fflush(stdout);
                    }
                }
            }
        }
    }
}

/*
 * vms-7c0: the p. 2-29 DATAGRAM input routine has deliberately NOT been
 * written, and this comment is the reason rather than an oversight.
 *
 * scs_rx.h carries the measurement: over 141 reference-lab pcaps and 981,367
 * envelope-conformant SCA frames the SCS message-type namespace is exactly
 * {0..10}, of which 10 is the p. 4-13 APPLICATION MESSAGE and 0..9 are the
 * connection-control messages. There is no eleventh value, and p. 4-68's
 * credit==0 rule is necessary rather than sufficient (24.1% of MTYPE-10 frames
 * carry credit 0), so no field OVMX can read off this wire says "datagram".
 *
 * Installing a datagram input routine would therefore install a routine that no
 * frame in any capture we hold can reach, and the choice of discriminator would
 * be a guess presented as a decode. scs_cdl_deliver_datagram() consequently
 * still has NO production caller after this item; the rx_unknown_mtype counter
 * at the delivery site is what will notice the first frame that could be one.
 */

/* vms-600: a private little-endian u32 reader, matching the read side of a
 * host buffer descriptor (sec 5.3: {offset, SCS buffer NAME, SCS connection
 * ID}, 12 bytes) the same way scs_mscp_srv.c's own static get_le32() does --
 * not exported, so each translation unit that needs it keeps its own copy
 * rather than sharing a wire-parsing helper across a module boundary. */
static uint32_t scsd_mscp_le32(const uint8_t *s)
{
    return (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16)
           | ((uint32_t)s[3] << 24);
}

/* vms-600: OVMX's own SCS buffer name for the source half of a block
 * transfer. OVMX never allocates a real SCS named buffer (scs_mscp_srv.h:
 * "no SYSAP-side buffer table -- OVMX only ever plays the served-block
 * side"), so this is an OVMX DESIGN CHOICE label, not a decode -- "OVM" in
 * ASCII, chosen only so a capture shows an obviously-synthetic value rather
 * than a suspiciously plausible one. */
#define OVMX_MSCP_SRV_XFER_SRC_BUFNAME 0x004F564Du

/*
 * vms-600: scsd_mscp_srv_xfer - THE LIVE scs_mscp_srv_xfer_fn (see its
 * typedef in scs_mscp_srv.h, design decision (4)). Called SYNCHRONOUSLY from
 * inside scs_mscp_srv_handle(), once per 512-byte block a READ moves --
 * scsd_mscp_srv_msg_input() populates scsd_mscp_xfer_ctx immediately before
 * that call and it stays valid for the whole of it, because this daemon is
 * single-threaded and synchronous (design decision (3)): there is never more
 * than one command in flight, so one static context is correct and costs no
 * allocation.
 *
 * Builds the 28-byte SCA block-transfer header (scs_mscp_srv.h "SCA BLOCK
 * DATA TRANSFER", vms-4e31) from the command's own host buffer descriptor
 * plus this transfer's running byte count, then sends it through
 * send_frame_vc() -- the SAME choke point every other SCS-layer sender in
 * this file uses (p. 2-31: no traffic on a circuit that is not OPEN). A
 * block-transfer frame is ordinary sequenced traffic on the class driver's
 * own connection, ADDRESSED using the SAME Con.ID pair as the end message
 * that will follow it, not a formation packet -- so it is CHOKED like every
 * other sequenced sender, not exempted. See this function's entry in the SEND
 * SITE TABLE above send_frame_vc().
 */
static long scsd_mscp_srv_xfer(void *ctx, const uint8_t buffer_desc[12],
                               uint32_t lbn, const uint8_t *data, size_t len)
{
    (void)lbn;
    struct scsd_mscp_srv_xfer_ctx *c = (struct scsd_mscp_srv_xfer_ctx *)ctx;
    if (c == NULL || c->ps == NULL || buffer_desc == NULL || data == NULL) {
        return -1;
    }

    struct scs_mscp_blk_hdr h;
    memset(&h, 0, sizeof(h));
    /* Appendix D / docs/design-mscp-direction.md's decoded buffer descriptor:
     * { u32 offset, u32 SCS buffer NAME, u32 SCS connection ID }. */
    h.dest_conid      = scsd_mscp_le32(buffer_desc + 8);
    h.f4              = c->conn_const;
    h.f6              = c->xfer_const;
    /* Down-counting, INCLUDING this frame's own data (docs/design-mscp-
     * direction.md's capture decode). */
    h.bytes_remaining = c->bytes_total - c->bytes_sent;
    h.src_buf_name    = c->src_buf_name;
    h.dest_offset     = scsd_mscp_le32(buffer_desc + 0) + c->bytes_sent;
    h.dest_buf_name   = scsd_mscp_le32(buffer_desc + 4);
    h.src_offset      = c->bytes_sent;

    struct scs_mscp_params p = c->tmpl;
    p.recv_ack = c->ps->vc.seq.recv_seq;
    p.send_seq = scs_seq_advance(&c->ps->vc.seq);

    uint8_t frame[SCS_ENV_ETH_HDR_LEN + SCS_MSCP_BLK_HDR_OFF
                  + SCS_MSCP_BLK_HDR_LEN + SCS_MSCP_BLOCK_SIZE];
    long flen = scs_mscp_srv_build_block_frame(&p, &h, data, len, frame,
                                               sizeof(frame));
    if (flen < 0) {
        return -1;
    }
    if (send_frame_vc(c->sock, c->ifindex, c->ps, c->ps->pb,
                      "MSCP block data (server)", frame, (size_t)flen) <= 0) {
        return -1;
    }
    c->bytes_sent += (uint32_t)len;
    return (long)len;
}

/*
 * vms-34b: scsd_mscp_srv_msg_input - THE LIVE MSCP DISK-SERVER RESPONDER's
 * message-input routine (p. 2-29). Installed on the CDT allocated at
 * OVMX_MSCP_SERVER_CONID the moment OVMX accepts a member-opened MSCP$DISK
 * connect (scsd_handle_frame()'s (b1.5) block, below). Every application
 * message (MTYPE 10) the class driver sends on that connection afterwards
 * reaches here through the SAME scs_cdl_deliver_message() path
 * scsd_sysap_msg_input() uses (the generic dispatch inside scsd_handle_frame)
 * -- this is what "an MSCP command responder exists" means for a LIVE
 * connection now, not only for scs_mscp_srv.c's own unit tests. Before this
 * item no CDT was ever allocated at OVMX_MSCP_SERVER_CONID (scs_mscp.h's own
 * comment on struct scs_mscp_params::cdt names this), so every command a real
 * class driver sent after the op=4 accept resolved SCS_DELIVER_NO_CDT and was
 * silently dropped -- exactly the facade "an MSCP command responder exists"
 * is supposed to rule out.
 *
 * `msg`/`msglen` are the p. 4-15 payload scs_cdl_deliver_message() already
 * extracted; this routine instead re-parses the RAW frame through
 * scsd_rx_current (the same OVMX DESIGN CHOICE scsd_sysap_msg_input() uses --
 * see its own comment), because scs_mscp_parse()'s contract is "a whole
 * received frame" and re-deriving a synthetic Ethernet+SCA header around
 * `msg` to satisfy it would be duplicate, divergeable work.
 *
 * WHETHER A BACKING STORE IS ATTACHED depends on OVMX_MSCP_SRV_UNIT_FILE
 * (scsd_mscp_srv_state()'s comment, vms-600). Unset -- every existing
 * ctest/CI configuration -- every command still gets scs_mscp_srv_handle()'s
 * honest "nothing is attached" answer (Unit-Offline / Invalid Command / Write
 * Protected), unchanged from vms-34b. Set, a READ against an online unit now
 * also drives scsd_mscp_srv_xfer() (above) to put real block data on the wire
 * before the end message below it -- either way a real end message, never
 * silence, which is the whole point.
 */
static void scsd_mscp_srv_msg_input(struct scs_cdt *cdt, const void *msg,
                                    size_t msglen, void *ctx)
{
    (void)cdt;
    (void)msg;
    (void)msglen;
    struct peer_state *ps = (struct peer_state *)ctx;
    struct scsd_rx_frame cur = scsd_rx_current;
    struct scs_mscp_view v;

    if (ps == NULL || cur.frame == NULL || cur.rx == NULL ||
        scs_mscp_parse(cur.frame, (size_t)cur.len, &v) != 0) {
        return; /* not a well-formed MSCP-over-SCS frame -- nothing safe to
                 * answer */
    }
    if (v.is_end) {
        /* sec 5.1: a controller ANSWERS commands, it does not receive end
         * messages. scs_mscp_srv_handle() would refuse this itself (returns
         * -1) for the same reason; caught here too so no body is even sliced
         * out of a frame that cannot be a command. */
        return;
    }

    uint8_t body[SCS_MSCP_BODY_LEN];
    memset(body, 0, sizeof(body));
    size_t hdr_off = (size_t)SCS_ENV_ETH_HDR_LEN + (size_t)SCS_MSCP_BODY_OFF;
    size_t avail = (size_t)cur.len > hdr_off ? (size_t)cur.len - hdr_off : 0;
    size_t take = avail < sizeof(body) ? avail : sizeof(body);
    memcpy(body, cur.frame + hdr_off, take);

    /* vms-600: prime the live xfer hook's scratch state BEFORE the call that
     * may invoke it. scs_mscp_srv_handle() reaches scsd_mscp_srv_xfer() only
     * from inside a READ against an online unit, but populating this
     * unconditionally (rather than branching on v.base_opcode here too) keeps
     * the "what the xfer hook sees" logic in exactly one place. */
    scsd_mscp_xfer_ctx.sock = cur.rx->sock;
    scsd_mscp_xfer_ctx.ifindex = cur.rx->ifindex;
    scsd_mscp_xfer_ctx.ps = ps;
    memset(&scsd_mscp_xfer_ctx.tmpl, 0, sizeof(scsd_mscp_xfer_ctx.tmpl));
    memcpy(scsd_mscp_xfer_ctx.tmpl.dst_mac, ps_port_addr(ps), 6);
    memcpy(scsd_mscp_xfer_ctx.tmpl.src_mac, cur.rx->our_hw_mac, 6);
    memcpy(scsd_mscp_xfer_ctx.tmpl.src_logical, cur.rx->our_src_logical, 6);
    memcpy(scsd_mscp_xfer_ctx.tmpl.peer_logical, ps_sys_addr(ps), 6);
    scsd_mscp_xfer_ctx.tmpl.remote_conid = ps->mscp_srv_remote_conid;
    scsd_mscp_xfer_ctx.tmpl.local_conid = PS_MSCP_SERVER_CONID(ps);
    scsd_mscp_xfer_ctx.tmpl.incarnation = ps->incarnation;
    scsd_mscp_xfer_ctx.bytes_total =
        (v.base_opcode == SCS_MSCP_OP_READ && !v.is_end
         && take >= (size_t)SCS_MSCP_P_BCNT + 4)
            ? scsd_mscp_le32(body + SCS_MSCP_P_BCNT)
            : 0;
    scsd_mscp_xfer_ctx.bytes_sent = 0;
    scsd_mscp_xfer_ctx.src_buf_name = OVMX_MSCP_SRV_XFER_SRC_BUFNAME;
    scsd_mscp_xfer_ctx.conn_const = 0; /* UNGROUNDED wire field -- scs_mscp_srv.h */
    scsd_mscp_xfer_ctx.xfer_const = ++scsd_mscp_xfer_generation;

    uint8_t end[SCS_MSCP_SRV_END_MAX];
    long endlen = scs_mscp_srv_handle(scsd_mscp_srv_state(),
                                      ps->mscp_srv_remote_conid, &v, body,
                                      sizeof(body), end, sizeof(end));
    if (endlen < 0) {
        return; /* scs_mscp_srv_handle()'s own "cannot answer at all" case --
                 * malformed/short input, not a command it declines (that path
                 * returns a well-formed end message instead, per its own
                 * contract). */
    }

    struct scs_mscp_params p;
    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, ps_port_addr(ps), 6);
    memcpy(p.src_mac, cur.rx->our_hw_mac, 6);
    memcpy(p.src_logical, cur.rx->our_src_logical, 6);
    memcpy(p.peer_logical, ps_sys_addr(ps), 6);
    /* THE DIRECTION: OVMX is the SERVER here, answering FROM its own server
     * handle TO the class driver's handle -- the opposite pairing from the
     * op=0/op=1/op=4 handshake above, where OVMX was echoing the MEMBER's
     * handle as "remote" while binding its own as local. Here OVMX is the one
     * addressing the frame, so its own handle is the SOURCE and the member's
     * is the DESTINATION. */
    p.remote_conid = ps->mscp_srv_remote_conid; /* destination: the class driver's handle */
    p.local_conid = PS_MSCP_SERVER_CONID(ps);   /* source: OVMX's own server handle */
    p.recv_ack = ps->vc.seq.recv_seq;
    p.send_seq = scs_seq_advance(&ps->vc.seq);
    p.incarnation = ps->incarnation;

    uint8_t frame[SCS_ENV_ETH_HDR_LEN + SCS_MSCP_BODY_OFF + SCS_MSCP_SRV_END_MAX];
    long flen = scs_mscp_srv_build_end_frame(&p, end, (size_t)endlen, frame,
                                             sizeof(frame));
    if (flen > 0) {
        (void)send_frame_vc(cur.rx->sock, cur.rx->ifindex, ps, ps->pb,
                            "MSCP end message (server)", frame, (size_t)flen);
    }
}

/* ======================================================================
 * vms-94c (DLM epic vms-7fa rung 1) -- the DLM SYSAP RECEIVE path.
 *
 * A dedicated SCS connection class for distributed-lock-manager traffic, like
 * MSCP$DISK and SCS$DIRECTORY. Rung 1 is the message TRANSPORT: a DLM message
 * (src/vmsscs/scs_dlm.c) that arrives on this node's DLM server handle
 * (PS_DLM_SERVER_CONID) is routed here by scs_cdl_deliver_message (dest Con.ID),
 * DECODED, and DISPATCHED to the executive's cross-node lock handler over
 * /dev/vms (VMS_IOCTL_DLM_XNODE -> vms_lock_dlm_xnode_dispatch), which returns
 * SS$_UNSUPPORTED in rung 1. The pipe is real; the grant is rung 2. INV-6:
 * nothing here fabricates a grant or a receipt -- a request that cannot be
 * dispatched fails honestly.
 * ====================================================================== */

static unsigned long rx_dlm_dispatched = 0; /* DLM frames decoded + dispatched */

/*
 * Held-lock readback (DLM rung 2 / vms-e8f1). After the master GRANTS an inbound
 * cross-node $ENQ, scsd_dlm_dispatch_to_executive() reads the master's OWN
 * resource DB back via VMS_IOCTL_GET_RESMASTER and fills this, so node B can
 * PROVE the grant is genuine: the resource exists, is mastered here, and holds a
 * lock FOR the remote requester's CSID -- not merely that the ENQ returned
 * SS$_NORMAL. `queried` is 1 iff the readback ran (only on a granted ENQ).
 */
struct scsd_dlm_held {
    int      queried;
    uint32_t found;
    uint32_t master_csid;
    uint32_t is_local_master;
    uint32_t n_granted;
    uint32_t held_for_csid;   /* remote_holder_csid: whose CSID the grant is for */
    uint32_t master_lkid;     /* the master's lock handle for the dispatched req
                               * (vms-6ca, H5): sent back so the requester can name
                               * the holder in a later $DEQ. */
    /* BLKAST WIRE (vms-76d, H6): when this ENQ QUEUED behind a cross-node holder,
     * the executive named the holder that must receive a BLKAST. blocking_req_lkid
     * is the holder's REQUESTER-side handle (the value the BLKAST frame carries so
     * the holder node finds its ORIGIN record); blocking_csid names the holder. */
    uint32_t blocking_csid;
    uint32_t blocking_req_lkid;
    /* LVB read crossing (rd vms-eeb, rung H9): the master's current 16-byte value
     * block, read back from the executive after a cross-node $ENQ grant and
     * carried into the GRANT reply frame so the requester's $ENQ reads the
     * master's LVB. A READ of real executive state (vms_dlm_xnode_args.valblk),
     * never fabricated (INV-6). */
    uint8_t  valblk[16];
};

/*
 * Deferred-GRANT report (DLM epic vms-7fa rung H5, vms-6ca). On a cross-node
 * $DEQ that flipped a QUEUED cross-node waiter to granted, the executive names
 * that waiter so node B can WIRE a deferred GRANT to the requester. `flipped`
 * is 1 iff a waiter flipped; the rest name it. A READ of the executive's genuine
 * decision, never fabricated (INV-6).
 */
struct scsd_dlm_defer {
    int      flipped;
    uint32_t req_csid;        /* requester to notify with the deferred GRANT */
    uint32_t master_lkid;     /* the flipped waiter's master lock handle      */
    uint32_t req_lkid;        /* the flipped waiter's ORIGINAL requester handle */
    uint32_t mode;            /* the mode it was granted at                    */
};

/*
 * The known 16-byte LVB node A writes on the H8 rung (rd vms-d81). A fixed,
 * addressable pattern so the host verdict can assert all three values equal --
 * the bytes A wrote, the bytes B read, and this expected pattern. Hex:
 * 4f564d584c56422d52554e4736000000 ("OVMXLVB-RUNG6" + 3 NULs). The run script's
 * H8_EXPECT_HEX MUST track this array (there is no shared C/shell header). INV-6:
 * the marker prints the REAL bytes lifted from the executive, never this literal.
 */
static const uint8_t scsd_h8_lvb[LCK$C_VALBLK_LEN] = {
    'O','V','M','X','L','V','B','-','R','U','N','G','6', 0, 0, 0
};

/* scsd_hex16 - lower-case hex-encode a 16-byte block into a 33-byte buffer. */
static void scsd_hex16(const uint8_t *b, char out[33])
{
    static const char h[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 16; i++) {
        out[2 * i]     = h[(b[i] >> 4) & 0xF];
        out[2 * i + 1] = h[b[i] & 0xF];
    }
    out[32] = '\0';
}

/*
 * scsd_dlm_local_read_valblk - node B's LOCAL LVB read (rd vms-d81, H8). After a
 * remote holder's cross-node $DEQ replicated its value block into the master
 * resource (res->valblk), this issues a LOCAL $ENQ (EX, LCK_M_VALBLK) on THIS
 * node for the same resource and returns res->valblk. The input value block is
 * left all-zero on purpose: the single-node $ENQ read path (vms_enq_core,
 * has_val==0) copies res->valblk INTO args.valblk rather than overwriting the
 * resource -- so this READS the LVB, it does not clobber it. Fail-honest
 * (SS$_NOSUCHDEV) with an all-zero block when /dev/vms is absent or the ENQ
 * fails (INV-6: an unset/failed read yields the real zero bytes, and the host
 * verdict then MISMATCHES A's write -> FAIL, never a vacuous pass).
 */
static uint32_t scsd_dlm_local_read_valblk(const char *resnam,
                                           uint8_t out[LCK$C_VALBLK_LEN])
{
    memset(out, 0, LCK$C_VALBLK_LEN);
#ifdef SCSD_UNIT_TEST
    (void)resnam;
    return 2296u;
#else
    struct vms_enq_args args;
    struct vms_register_args reg;
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0)
        return 2680u;
    memset(&reg, 0, sizeof(reg));
    if (ioctl(fd, VMS_IOCTL_REGISTER, &reg) < 0) { close(fd); return 2680u; }

    memset(&args, 0, sizeof(args));
    args.lkmode = LCK$K_EXMODE;
    args.flags = LCK_M_VALBLK;      /* read the LVB back (input block all-zero) */
    if (resnam) {
        strncpy(args.resnam, resnam, sizeof(args.resnam) - 1);
        args.resnam[sizeof(args.resnam) - 1] = '\0';
    }
    if (ioctl(fd, VMS_IOCTL_ENQ, &args) < 0) { close(fd); return 2680u; }
    if (args.status == 1u)
        memcpy(out, args.valblk, LCK_VALBLK_SIZE);
    uint32_t status = args.status;
    close(fd);
    return status;
#endif
}

/*
 * scsd_dlm_dispatch_to_executive - hand ONE decoded cross-node DLM request to
 * the kernel lock manager over /dev/vms. Returns the executive status: a granted
 * cross-node $ENQ now yields SS$_NORMAL (1) (DLM rung 2, vms-e8f1); a later-rung
 * op (DEQ/GRANT/BLKAST) yields SS$_UNSUPPORTED (2296). FAIL-HONEST (Rule 9 /
 * INV-6): if /dev/vms cannot be opened or the ioctl fails, returns SS$_NOSUCHDEV
 * (2680) -- it NEVER fabricates a grant. scsd is a glibc process, so it issues a
 * direct POSIX ioctl here rather than the freestanding vms_kif client.
 *
 * `held` (may be NULL): on a GRANTED ENQ, filled from a follow-up GET_RESMASTER
 * on the SAME registered fd so the caller can print the held-lock proof.
 */
static uint32_t scsd_dlm_dispatch_to_executive(const struct scs_dlm_msg *m,
                                               struct scsd_dlm_held *held,
                                               struct scsd_dlm_defer *defer,
                                               uint32_t *out_origin_mode)
{
    if (held)
        memset(held, 0, sizeof(*held));
    if (defer)
        memset(defer, 0, sizeof(*defer));
    if (out_origin_mode)
        *out_origin_mode = 0;
#ifdef SCSD_UNIT_TEST
    /* Capture seam, parallel to send_frame_raw's: record the decoded request so
     * a test can assert node B decoded + dispatched the right fields. The real
     * ioctl needs /dev/vms, which CI does not have. */
    scsd_test_last_dlm = *m;
    scsd_test_dlm_dispatches++;
    return 2296u; /* SS$_UNSUPPORTED, as the real handler returns in rung 1 */
#else
    struct vms_dlm_xnode_args args;
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0)
        return 2680u; /* SS$_NOSUCHDEV -- fail honest, no fake grant */

    /* REGISTER first (vms-4b6). The executive's ioctl dispatcher gates EVERY
     * ioctl behind vms_proc_find_or_err() (src/kernel/vms_module.c) -- a caller
     * with no registered VMS process context is refused -ESRCH before the DLM
     * cross-node handler is ever reached. VMS_IOCTL_DLM_XNODE itself needs no
     * registered proc (it delivers a peer's request, not the daemon's own lock),
     * but the DEVICE does: talking to /dev/vms at all requires the standard
     * REGISTER handshake every other client performs (see
     * tests/qemu/test_syssvc_dlm_xnode.c, which REGISTERs before it dispatches).
     * Without this, scsd's dispatch reached the device but was rejected before
     * the handler, so a REAL executive returned SS$_NOSUCHDEV(2680) exactly like
     * a missing device -- the two failures were indistinguishable and the
     * cross-node receive path could never actually reach the lock manager. The
     * DLM harness rung H0 (a real /dev/vms) surfaced this; the Docker harness
     * masked it (no device, always 2680). Keying is by process (current->tgid),
     * so this registers scsd once and is idempotent across the fresh fds this
     * function opens per call. INV-6: registration grants nothing -- it only
     * establishes the caller context the executive requires. */
    struct vms_register_args reg;
    memset(&reg, 0, sizeof(reg));
    if (ioctl(fd, VMS_IOCTL_REGISTER, &reg) < 0) {
        close(fd);
        return 2680u; /* device present but the executive refused us -- honest */
    }

    memset(&args, 0, sizeof(args));
    args.op = m->op;
    args.lkmode = m->mode;
    args.flags = m->flags;
    args.req_lkid = m->req_lkid;
    args.master_lkid = m->master_lkid;
    args.req_csid = m->req_csid;
    args.master_csid = m->master_csid;
    {
        size_t n = m->namelen;
        if (n > sizeof(args.resnam) - 1)
            n = sizeof(args.resnam) - 1;
        memcpy(args.resnam, m->resnam, n);
        args.resnam[sizeof(args.resnam) - 1] = '\0';
    }
    memcpy(args.valblk, m->valblk, sizeof(args.valblk));
    if (ioctl(fd, VMS_IOCTL_DLM_XNODE, &args) < 0) {
        close(fd);
        return 2680u; /* honest failure, not a grant */
    }

    /* The master's lock handle for the dispatched request (vms-6ca, H5), so the
     * requester can name the holder in a later cross-node $DEQ. */
    if (held != NULL)
        held->master_lkid = args.master_lkid;

    /* LVB read crossing (rd vms-eeb, rung H9): carry the master's value block,
     * which the executive returned in args.valblk after resolving this cross-node
     * $ENQ (the read of res->valblk on grant), up to the GRANT reply builder so
     * it rides back to the requester. A READ of real executive state, never faked. */
    if (held != NULL)
        memcpy(held->valblk, args.valblk, sizeof(held->valblk));

    /* BLKAST WIRE (vms-76d, H6): on an ENQ that QUEUED behind a cross-node holder,
     * carry the executive's blocking-AST directive up so the server leg can WIRE a
     * BLKAST to that holder. A READ of the executive's genuine decision (INV-6). */
    if (held != NULL && args.op == VMS_DLM_OP_ENQ && args.queued == 1u) {
        held->blocking_csid = args.blocking_csid;
        held->blocking_req_lkid = args.blocking_req_lkid;
    }

    /* DLM rung 2 (vms-e8f1): if the master GRANTED this cross-node $ENQ, read its
     * OWN resource DB back on the SAME registered fd so the caller can prove the
     * grant is genuine -- the resource is mastered here and a lock is held FOR
     * the remote requester's CSID (remote_holder_csid). This is a READ of the
     * executive's real lock state, never a fabrication (INV-6). */
    if (held != NULL && args.op == VMS_DLM_OP_ENQ && args.status == 1u) {
        struct vms_resmaster_args rm;
        memset(&rm, 0, sizeof(rm));
        memcpy(rm.resnam, args.resnam, sizeof(rm.resnam));
        rm.resnam[sizeof(rm.resnam) - 1] = '\0';
        if (ioctl(fd, VMS_IOCTL_GET_RESMASTER, &rm) == 0) {
            held->queried = 1;
            held->found = rm.found;
            held->master_csid = rm.master_csid;
            held->is_local_master = rm.is_local_master;
            held->n_granted = rm.n_granted;
            held->held_for_csid = rm.remote_holder_csid;
        }
    }

    /* DLM rung H5 (vms-6ca): a cross-node $DEQ that flipped a queued waiter to
     * granted names it in the fields a DEQ otherwise leaves 0, so the caller can
     * WIRE the deferred GRANT to that requester. A READ of the executive's real
     * report, never fabricated (INV-6). */
    if (defer != NULL && args.op == VMS_DLM_OP_DEQ && args.queued == 1u) {
        defer->flipped = 1;
        defer->req_csid = args.blocking_csid;
        defer->master_lkid = args.blocking_master_lkid;
        defer->req_lkid = args.req_lkid;
        defer->mode = args.lkmode;
    }

    /* DLM rung H5 (vms-6ca): the REQUESTER-SIDE GRANT RECEIVE completed our
     * executive-resident ORIGIN record. Read the record BACK via GETLKI on the
     * SAME registered fd so the caller can prove the status the master sent
     * genuinely landed in executive state -- the NL->EX flip observed on the
     * requester node, not a userspace flag (INV-6). */
    if (out_origin_mode != NULL && args.op == VMS_DLM_OP_GRANT &&
        args.status == 1u && args.req_lkid != 0) {
        struct vms_getlki_args gl;
        memset(&gl, 0, sizeof(gl));
        gl.lkid = args.req_lkid;
        if (ioctl(fd, VMS_IOCTL_GETLKI, &gl) == 0 && gl.status == 1u)
            *out_origin_mode = gl.granted_mode;
    }

    close(fd);
    return args.status;
#endif
}

/*
 * scsd_dlm_blkast_handler - node A's holder-side blocking-AST routine (vms-76d,
 * H6). Its ADDRESS is what node A registers as the holder's blkastadr; a real
 * user-mode function so the address round-trips genuinely through the executive's
 * AST queue and DELIVERAST. It is never called in the harness (delivery is proven
 * by DRAINING the queued AST, not by an in-process trap), but it must be a real,
 * addressable routine -- not a fabricated pointer.
 */
static volatile unsigned long scsd_dlm_blkast_calls;
static void scsd_dlm_blkast_handler(unsigned long astprm)
{
    scsd_dlm_blkast_calls += astprm ? astprm : 1;
}

/*
 * scsd_dlm_holder_establish - node A's HOLDER ORIGIN, WITH a blocking-AST routine
 * (vms-76d, H6). Dispatches a GRANT receive (mode EX, blkastadr set) into A's
 * executive so the holder (#1) origin record can later receive a real BLKAST.
 * Mirrors scsd_dlm_dispatch_to_executive's REGISTER handshake. Returns the exec
 * status; fail-honest (SS$_NOSUCHDEV) when /dev/vms is absent (INV-6).
 */
static uint32_t scsd_dlm_holder_establish(uint32_t req_lkid, uint8_t mode,
                                          uint32_t master_lkid, const char *resnam,
                                          uint64_t blkastadr)
{
#ifdef SCSD_UNIT_TEST
    (void)req_lkid; (void)mode; (void)master_lkid; (void)resnam; (void)blkastadr;
    return 2296u;
#else
    struct vms_dlm_xnode_args args;
    struct vms_register_args reg;
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0)
        return 2680u;
    memset(&reg, 0, sizeof(reg));
    if (ioctl(fd, VMS_IOCTL_REGISTER, &reg) < 0) { close(fd); return 2680u; }

    memset(&args, 0, sizeof(args));
    args.op = VMS_DLM_OP_GRANT;
    args.lkmode = mode;
    args.req_lkid = req_lkid;
    args.master_lkid = master_lkid;
    args.req_csid = resolve_scssystemid();
    args.blkastadr = blkastadr;
    args.blkastprm = (uint64_t)req_lkid;
    if (resnam) {
        strncpy(args.resnam, resnam, sizeof(args.resnam) - 1);
        args.resnam[sizeof(args.resnam) - 1] = '\0';
    }
    if (ioctl(fd, VMS_IOCTL_DLM_XNODE, &args) < 0) { close(fd); return 2680u; }
    close(fd);
    return args.status;
#endif
}

/*
 * scsd_dlm_blkast_fire - node A's holder-side BLKAST RECEIVE (vms-76d, H6). A
 * BLKAST node B sent over SCS is dispatched into A's executive, which FIRES the
 * holder's blocking AST for real; this then DRAINS the queued AST via DELIVERAST
 * and returns the drained routine address, PROVING the AST genuinely landed on A's
 * process (INV-6: the executive really queued a user-mode AST, not a log line).
 * *out_delivered = the executive's blkast_delivered flag; *out_drained = the
 * astadr DELIVERAST handed back (0 if none). Returns the exec status.
 */
static uint32_t scsd_dlm_blkast_fire(uint32_t req_lkid, const char *resnam,
                                     uint32_t *out_delivered,
                                     uint64_t *out_drained)
{
    if (out_delivered) *out_delivered = 0;
    if (out_drained) *out_drained = 0;
#ifdef SCSD_UNIT_TEST
    (void)req_lkid; (void)resnam;
    return 2296u;
#else
    struct vms_dlm_xnode_args args;
    struct vms_register_args reg;
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0)
        return 2680u;
    memset(&reg, 0, sizeof(reg));
    if (ioctl(fd, VMS_IOCTL_REGISTER, &reg) < 0) { close(fd); return 2680u; }

    memset(&args, 0, sizeof(args));
    args.op = VMS_DLM_OP_BLKAST;
    args.req_lkid = req_lkid;
    args.req_csid = resolve_scssystemid();
    if (resnam) {
        strncpy(args.resnam, resnam, sizeof(args.resnam) - 1);
        args.resnam[sizeof(args.resnam) - 1] = '\0';
    }
    if (ioctl(fd, VMS_IOCTL_DLM_XNODE, &args) < 0) { close(fd); return 2680u; }
    if (out_delivered) *out_delivered = args.blkast_delivered;

    /* DRAIN the AST the BLKAST just queued -- proof it genuinely landed. Same
     * registered proc (keyed by tgid), so the AST queued above is drainable here. */
    if (args.status == 1u && args.blkast_delivered) {
        struct vms_ast_args ast;
        memset(&ast, 0, sizeof(ast));
        if (ioctl(fd, VMS_IOCTL_DELIVERAST, &ast) == 0 && out_drained)
            *out_drained = ast.astadr;
    }
    uint32_t status = args.status;
    close(fd);
    return status;
#endif
}

/*
 * H9 known LVB pattern (rd vms-eeb) -- "OVMXLVB-READ9" NUL-padded to 16 bytes.
 * DISTINCT from the H8 write-crossing pattern (vms-d81 used "OVMXLVB-RUNG6"), so a
 * cross-run bleed cannot masquerade as a pass. The host runner pins the SAME 32
 * hex digits (H9_EXPECT_HEX) and asserts A_read == B_seed == this pattern.
 */
static const uint8_t scsd_h9_known_block[16] = {
    'O','V','M','X','L','V','B','-','R','E','A','D','9', 0, 0, 0
};

/*
 * scsd_dlm_h9_seed_valblk - node B (the master) SEEDS RRD's resource value block
 * (rd vms-eeb, rung H9). A LOCAL $ENQ (EX, LCK_M_VALBLK) with a NON-ZERO input
 * block writes it to res->valblk (the single-node core, vms_lock.c: on grant with
 * a user-supplied block, res->valblk <- lock->valblk); a LOCAL $DEQ (LCK_M_VALBLK,
 * same block) then releases the lock while res->valblk PERSISTS -- the core keeps
 * a resource whose value block is non-zero. So a later cross-node $ENQ from node A
 * reads the seeded LVB back. out_readback receives the block the executive confirms
 * it granted (its REAL res->valblk), never a fabricated one (INV-6). Fail-honest
 * (SS$_NOSUCHDEV) when /dev/vms is absent.
 */
static uint32_t scsd_dlm_h9_seed_valblk(const char *resnam,
                                        const uint8_t *block,
                                        uint8_t *out_readback)
{
    if (out_readback)
        memset(out_readback, 0, LCK$C_VALBLK_LEN);
#ifdef SCSD_UNIT_TEST
    (void)resnam; (void)block;
    return 2296u; /* SS$_UNSUPPORTED, as the rung-1 stubs return */
#else
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0)
        return 2680u; /* SS$_NOSUCHDEV -- honest, no fake seed */
    struct vms_register_args reg;
    memset(&reg, 0, sizeof(reg));
    if (ioctl(fd, VMS_IOCTL_REGISTER, &reg) < 0) { close(fd); return 2680u; }

    /* LOCAL $ENQ RRD (EX, VALBLK) with the known block: writes it to res->valblk. */
    struct vms_enq_args e;
    memset(&e, 0, sizeof(e));
    e.lkmode = LCK_K_EXMODE;
    e.flags  = LCK_M_VALBLK;
    strncpy(e.resnam, resnam, sizeof(e.resnam) - 1);
    e.resnam[sizeof(e.resnam) - 1] = '\0';
    memcpy(e.valblk, block, LCK_VALBLK_SIZE);
    if (ioctl(fd, VMS_IOCTL_ENQ, &e) < 0 || e.status != 1u) {
        uint32_t st = e.status ? e.status : 2680u;
        close(fd);
        return st;
    }
    if (out_readback)
        memcpy(out_readback, e.valblk, LCK_VALBLK_SIZE);

    /* LOCAL $DEQ RRD (VALBLK, same non-zero block): release the lock; res->valblk
     * persists (a non-zero LVB keeps the resource alive across the DEQ). */
    struct vms_deq_args d;
    memset(&d, 0, sizeof(d));
    d.lkid  = e.lkid;
    d.flags = LCK_M_VALBLK;
    memcpy(d.valblk, block, LCK_VALBLK_SIZE);
    (void)ioctl(fd, VMS_IOCTL_DEQ, &d);

    close(fd);
    return e.status; /* SS$_NORMAL on a real grant */
#endif
}

/*
 * scsd_dlm_h9_read_valblk - node A reads back the LVB the master's GRANT delivered
 * (rd vms-eeb, rung H9). A LOCAL GETLKI on node A's OWN request handle (the
 * req_lkid it enqueued with) surfaces the value block grant_recv stored on the
 * origin record -- the master's res->valblk, transported B->A over SCS on the
 * GRANT. out_valblk receives the REAL executive bytes (zeros if the executive
 * never stored an LVB -> the runner FAILs, correctly). Fail-honest when /dev/vms
 * is absent.
 */
static uint32_t scsd_dlm_h9_read_valblk(uint32_t lkid, uint8_t *out_valblk)
{
    if (out_valblk)
        memset(out_valblk, 0, LCK$C_VALBLK_LEN);
#ifdef SCSD_UNIT_TEST
    (void)lkid;
    return 2296u;
#else
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0)
        return 2680u;
    struct vms_register_args reg;
    memset(&reg, 0, sizeof(reg));
    if (ioctl(fd, VMS_IOCTL_REGISTER, &reg) < 0) { close(fd); return 2680u; }
    struct vms_getlki_args gl;
    memset(&gl, 0, sizeof(gl));
    gl.lkid = lkid;
    if (ioctl(fd, VMS_IOCTL_GETLKI, &gl) < 0) { close(fd); return 2680u; }
    if (out_valblk)
        memcpy(out_valblk, gl.valblk, LCK_VALBLK_SIZE);
    close(fd);
    return gl.status;
#endif
}

/*
 * scsd_dlm_h9_seed - node B's one-shot LVB seed (rd vms-eeb, rung H9), fired from
 * the join retx tick once this node is a cluster member. Only the master (node B:
 * OVMX_DLM_H9 set, OVMX_DLM_ENQ UNSET) seeds; it runs ONCE (static latch) and BEFORE
 * node A's cross-node $ENQ can arrive (A's $ENQ waits on A's own join). Emits
 * SCSD-I-DLMLVBSEED with the REAL seeded LVB hex-encoded verbatim (INV-6).
 */
static void scsd_dlm_h9_seed(struct scsd_rx *rx, struct peer_state *ps)
{
    static int h9_seeded = 0;
    if (rx == NULL || ps == NULL)
        return;
    if (getenv("OVMX_DLM_H9") == NULL || getenv("OVMX_DLM_ENQ") != NULL)
        return; /* only the master/server side seeds */
    if (h9_seeded || !scsd_member_initiate_enabled())
        return;

    uint8_t readback[LCK$C_VALBLK_LEN];
    uint32_t st = scsd_dlm_h9_seed_valblk("RRD", scsd_h9_known_block, readback);
    h9_seeded = 1; /* one-shot regardless of outcome; a failure emits real zeros */

    char hex[33];
    scsd_hex16(readback, hex);
    log_ts(stdout);
    printf(" SCSD-I-DLMLVBSEED name=RRD val=%s (LOCAL $ENQ EX+VALBLK then $DEQ,"
           " rc=0x%08X) -- master seeded RRD's resource value block; res->valblk"
           " PERSISTS for node A's cross-node $ENQ to read (H9, the read crossing)\n",
           hex, (unsigned)st);
    fflush(stdout);
}

/* ============================================================================
 * DLM rung H10a (rd vms-2bf) -- GRACEFUL-DEPARTURE DIRECTORY INGRESS.
 *
 * When scsd observes a GRACEFUL cluster departure (SCS_MEMBER_OP_DEPART, the
 * class-0x04 self-departure a leaving node opens on the VC), it hands THIS
 * node's executive the departed CSID via the LOCAL ioctl
 * VMS_IOCTL_DLM_MEMBER_DEPART. The executive drops that CSID from the LIVE DLM
 * directory membership and invalidates cached directories, so a resource whose
 * directory hashed to the departed node DETERMINISTICALLY re-resolves to a
 * survivor (dlm_directory_csid re-hashes over the shrunk set). This is a LOCAL
 * ioctl to our OWN /dev/vms -- it adds NO new cross-node send (no send_frame_vc
 * site); the cross-node lock-STATE rebuild (COLLECT) is the H10b rung.
 *
 * The departed CSID is READ from the departing peer's own wire identity: its SCA
 * src-logical address is aa:00:04:00:<LE16(SCSSYSTEMID)> (scs_config.h), and the
 * H10 harness sets each node's SCSSYSTEMID equal to its DLM CSID, so
 * peer_node_number(ps) -- the low 16 bits of that address -- IS the departing
 * node's DLM CSID. INV-6: the markers carry the REAL executive-returned values
 * (found/live/dir_before/dir_after), never fabricated; fail-honest with no
 * /dev/vms (SS$_NOSUCHDEV 2680).
 * ==========================================================================*/

/*
 * scsd_dlm_h10_read_dir - LOCAL GET_RESMASTER: report the executive's directory
 * CSID for a resource NAME. dir_csid is a pure property of (name, live
 * membership) -- the resource need not exist (see vms_ioctl_get_resmaster). On
 * success *out_dir/*out_local carry the REAL dir_csid/local_csid. Fail-honest.
 */
static uint32_t scsd_dlm_h10_read_dir(const char *resnam, uint32_t *out_dir,
                                      uint32_t *out_local)
{
    if (out_dir) *out_dir = 0;
    if (out_local) *out_local = 0;
#ifdef SCSD_UNIT_TEST
    (void)resnam;
    return 2296u;
#else
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0)
        return 2680u; /* SS$_NOSUCHDEV -- honest, no fabricated directory */
    struct vms_register_args reg;
    memset(&reg, 0, sizeof(reg));
    if (ioctl(fd, VMS_IOCTL_REGISTER, &reg) < 0) { close(fd); return 2680u; }
    struct vms_resmaster_args rm;
    memset(&rm, 0, sizeof(rm));
    strncpy(rm.resnam, resnam, sizeof(rm.resnam) - 1);
    rm.resnam[sizeof(rm.resnam) - 1] = '\0';
    if (ioctl(fd, VMS_IOCTL_GET_RESMASTER, &rm) < 0) { close(fd); return 2680u; }
    if (out_dir)   *out_dir   = rm.dir_csid;
    if (out_local) *out_local = rm.local_csid;
    uint32_t st = rm.status;
    close(fd);
    return st;
#endif
}

/*
 * scsd_dlm_member_depart_ioctl - LOCAL VMS_IOCTL_DLM_MEMBER_DEPART: tell our
 * executive that departed_csid left the cluster. *out_live/*out_found receive the
 * executive's REAL post-shrink live directory-member count + the found flag
 * (1 iff departed_csid was a configured member). Fail-honest (SS$_NOSUCHDEV).
 */
static uint32_t scsd_dlm_member_depart_ioctl(uint32_t departed_csid,
                                             uint32_t *out_live,
                                             uint32_t *out_found)
{
    if (out_live)  *out_live  = 0;
    if (out_found) *out_found = 0;
#ifdef SCSD_UNIT_TEST
    (void)departed_csid;
    return 2296u;
#else
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0)
        return 2680u; /* SS$_NOSUCHDEV -- honest, no fabricated membership change */
    struct vms_register_args reg;
    memset(&reg, 0, sizeof(reg));
    if (ioctl(fd, VMS_IOCTL_REGISTER, &reg) < 0) { close(fd); return 2680u; }
    struct vms_dlm_depart_args da;
    memset(&da, 0, sizeof(da));
    da.departed_csid = departed_csid;
    if (ioctl(fd, VMS_IOCTL_DLM_MEMBER_DEPART, &da) < 0) { close(fd); return 2680u; }
    if (out_live)  *out_live  = da.members_live;
    if (out_found) *out_found = da.found;
    uint32_t st = da.status;
    close(fd);
    return st;
#endif
}

/*
 * scsd_cluster_member_set_ioctl - LOCAL VMS_IOCTL_CLUSTER_MEMBER_SET (rd
 * vms-551): tell our executive to insert-or-update this member in the
 * cluster-membership block (docs/design-cluster-membership-executive.md).
 * Same direct-POSIX-ioctl shape as scsd_dlm_member_depart_ioctl above --
 * this is a LOCAL ioctl to our OWN /dev/vms, not a new SCS wire send.
 * Fail-honest (SS$_NOSUCHDEV).
 */
static uint32_t scsd_cluster_member_set_ioctl(uint32_t csid, uint32_t sysid,
                                              const char *scsnode,
                                              const char *state)
{
#ifdef SCSD_UNIT_TEST
    (void)csid; (void)sysid; (void)scsnode; (void)state;
    return 2296u;
#else
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0)
        return 2680u; /* SS$_NOSUCHDEV -- honest, no fabricated membership */
    struct vms_register_args reg;
    memset(&reg, 0, sizeof(reg));
    if (ioctl(fd, VMS_IOCTL_REGISTER, &reg) < 0) { close(fd); return 2680u; }
    struct vms_cluster_member_set_args sa;
    memset(&sa, 0, sizeof(sa));
    sa.csid = csid;
    sa.sysid = sysid;
    if (scsnode)
        strncpy(sa.scsnode, scsnode, sizeof(sa.scsnode) - 1);
    if (state)
        strncpy(sa.state, state, sizeof(sa.state) - 1);
    if (ioctl(fd, VMS_IOCTL_CLUSTER_MEMBER_SET, &sa) < 0) { close(fd); return 2680u; }
    uint32_t st = sa.status;
    close(fd);
    return st;
#endif
}

/*
 * scsd_cluster_member_clear_ioctl - LOCAL VMS_IOCTL_CLUSTER_MEMBER_CLEAR (rd
 * vms-551): tell our executive this csid left. Idempotent -- an absent csid
 * is SS$_NORMAL, not an error. Same shape as the SET helper above.
 */
static uint32_t scsd_cluster_member_clear_ioctl(uint32_t csid)
{
#ifdef SCSD_UNIT_TEST
    (void)csid;
    return 2296u;
#else
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0)
        return 2680u; /* SS$_NOSUCHDEV -- honest, no fabricated membership */
    struct vms_register_args reg;
    memset(&reg, 0, sizeof(reg));
    if (ioctl(fd, VMS_IOCTL_REGISTER, &reg) < 0) { close(fd); return 2680u; }
    struct vms_cluster_member_clear_args ca;
    memset(&ca, 0, sizeof(ca));
    ca.csid = csid;
    if (ioctl(fd, VMS_IOCTL_CLUSTER_MEMBER_CLEAR, &ca) < 0) { close(fd); return 2680u; }
    uint32_t st = ca.status;
    close(fd);
    return st;
#endif
}

/*
 * Node A's H10a readback baseline. scsd_dlm_h10_before_read latches, once, the
 * directory CSID of the tracked resource (OVMX_DLM_H10_RES) over the FULL
 * membership -- BEFORE any departure. scsd_dlm_h10_depart_ingress reads it again
 * AFTER the departure ioctl shrank the membership and emits the before/after
 * remaster proof. Both values are REAL executive reads (INV-6).
 */
static char     scsd_h10_res[32];
static uint32_t scsd_h10_dir_before;
static int      scsd_h10_before_latched;

static void scsd_dlm_h10_before_read(void)
{
    const char *res = getenv("OVMX_DLM_H10_RES");
    if (res == NULL || res[0] == '\0')
        return;                 /* only the readback node (node A) arms this */
    if (scsd_h10_before_latched || !scsd_member_initiate_enabled())
        return;                 /* one-shot, and only in member-driving mode */

    uint32_t dir = 0, local = 0;
    uint32_t st = scsd_dlm_h10_read_dir(res, &dir, &local);
    strncpy(scsd_h10_res, res, sizeof(scsd_h10_res) - 1);
    scsd_h10_res[sizeof(scsd_h10_res) - 1] = '\0';
    scsd_h10_dir_before = dir;
    scsd_h10_before_latched = 1;   /* one-shot regardless of outcome */

    log_ts(stdout);
    printf(" SCSD-I-DLMDIRBEFORE name=%s dir_before=%u local=%u (rc=0x%08X)"
           " -- directory of %s over the FULL DLM membership, read before any"
           " departure (H10a readback baseline)\n",
           scsd_h10_res, (unsigned)dir, (unsigned)local, (unsigned)st, scsd_h10_res);
    fflush(stdout);
}

/* Definition (forward-declared above): the graceful-departure directory ingress. */
static void scsd_dlm_h10_depart_ingress(struct peer_state *ps)
{
    if (ps == NULL)
        return;
    if (getenv("OVMX_DLM_H10") == NULL)
        return;                 /* armed only in the H10 harness (real DLM membership) */

    /* The departing peer's DLM CSID == the low 16 bits of its SCA src-logical
     * address (aa:00:04:00:<LE16(SCSSYSTEMID)>), which the H10 harness sets equal
     * to the DLM CSID. A real departed CSID, read off the departing node's own
     * wire identity -- never fabricated. */
    uint32_t departed = peer_node_number(ps);
    if (departed == 0)
        return;                 /* no real departed CSID -> nothing to shrink */

    /* One-shot per departed CSID: a retransmitted class-0x04 open must not
     * re-drive the ioctl or re-emit the markers. Sized to the executive's
     * VMS_DLM_MAX_MEMBERS (16, vms_internal.h) -- a userspace-local copy, as that
     * kernel constant is not exposed through vms_ioctl.h. */
    enum { SCSD_H10_MAX_DEPARTED = 16 };
    static uint32_t seen[SCSD_H10_MAX_DEPARTED];
    static int      nseen;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == departed)
            return;
    if (nseen < (int)(sizeof(seen) / sizeof(seen[0])))
        seen[nseen++] = departed;

    uint32_t live = 0, found = 0;
    uint32_t st = scsd_dlm_member_depart_ioctl(departed, &live, &found);
    log_ts(stdout);
    printf(" SCSD-I-DLMDEPART csid=%u live=%u found=%u (rc=0x%08X) -- graceful"
           " class-0x04 departure ingress: executive dropped the CSID from the LIVE"
           " DLM directory membership (H10a, rd vms-2bf)\n",
           (unsigned)departed, (unsigned)live, (unsigned)found, (unsigned)st);
    fflush(stdout);

    /* Readback node (node A, OVMX_DLM_H10_RES set): re-read the tracked
     * resource's directory now the membership shrank, and emit the before/after
     * remaster proof. Gated on found==1 so we only claim a remaster after a REAL
     * membership shrink. */
    if (found == 1u && scsd_h10_before_latched && scsd_h10_res[0] != '\0') {
        uint32_t dir_after = 0, local = 0;
        uint32_t rst = scsd_dlm_h10_read_dir(scsd_h10_res, &dir_after, &local);
        log_ts(stdout);
        printf(" SCSD-I-DLMREMASTER name=%s dir_before=%u dir_after=%u"
               " (departed=%u local=%u rc=0x%08X) -- the resource's directory"
               " DETERMINISTICALLY re-resolved to a survivor after the departure"
               " (H10a directory re-resolution; lock-state rebuild is H10b)\n",
               scsd_h10_res, (unsigned)scsd_h10_dir_before, (unsigned)dir_after,
               (unsigned)departed, (unsigned)local, (unsigned)rst);
        fflush(stdout);
    }
}

/* ============================================================================
 * DLM rung H10b (rd vms-dca9) -- THE REMASTER LOCK REBUILD.
 *
 * H10a proves the departure INGRESS (membership shrink + deterministic
 * directory re-resolution). H10b proves that a cross-node LOCK survives the
 * departure of its master: node A holds RES_C EX (mastered by C) BEFORE C
 * departs; when C departs gracefully, A re-registers that SAME lock -- its
 * REAL req_lkid/mode/req_csid, read fresh off its own executive-resident
 * origin record, never assumed -- on RES_C's NEW directory master (a TARGETED
 * SCS_DLM_OP_REBUILD send, resolved by peer_by_csid, not the untargeted
 * per-peer sends every earlier DLM rung used). The new master reconstructs
 * res->granted DIRECTLY from that real state (vms_lock_dlm_xnode_rebuild,
 * src/kernel-core/vms_lock.c) and the harness VALUE-VERIFIES the rebuilt lock
 * -- read back via VMS_IOCTL_DLM_GET_GRANTED -- equals the one A held
 * pre-departure. INV-6: every value on the wire and in every marker is a REAL
 * executive read; a failed dispatch or an absent peer is reported honestly
 * (rc field / sent=0), never faked as a rebuild that did not happen.
 * ==========================================================================*/

/*
 * scsd_dlm_h10b_read_own_mode - LOCAL GETLKI: the REAL granted mode our own
 * origin record holds for `lkid`, read fresh at rebuild time (never the mode we
 * assumed when we sent the original $ENQ). Mirrors scsd_dlm_h9_read_valblk's
 * REGISTER+GETLKI shape. Fail-honest (SS$_NOSUCHDEV) with no /dev/vms.
 */
static uint32_t scsd_dlm_h10b_read_own_mode(uint32_t lkid, uint32_t *out_mode)
{
    if (out_mode)
        *out_mode = 0;
#ifdef SCSD_UNIT_TEST
    (void)lkid;
    return 2296u;
#else
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0)
        return 2680u; /* SS$_NOSUCHDEV -- honest, no fabricated mode */
    struct vms_register_args reg;
    memset(&reg, 0, sizeof(reg));
    if (ioctl(fd, VMS_IOCTL_REGISTER, &reg) < 0) { close(fd); return 2680u; }
    struct vms_getlki_args gl;
    memset(&gl, 0, sizeof(gl));
    gl.lkid = lkid;
    if (ioctl(fd, VMS_IOCTL_GETLKI, &gl) < 0) { close(fd); return 2680u; }
    if (out_mode)
        *out_mode = gl.granted_mode;
    uint32_t st = gl.status;
    close(fd);
    return st;
#endif
}

/*
 * scsd_dlm_h10b_get_granted - LOCAL VMS_IOCTL_DLM_GET_GRANTED: the new master's
 * readback of the first remote-held granted lock on `resnam` -- the value-verify
 * proof that the rebuild genuinely landed in res->granted. Fail-honest, all
 * fields zero with no /dev/vms.
 */
static uint32_t scsd_dlm_h10b_get_granted(const char *resnam, uint32_t *out_found,
                                          uint32_t *out_n_granted,
                                          uint32_t *out_holder_csid,
                                          uint32_t *out_holder_lkid,
                                          uint32_t *out_mode)
{
    if (out_found) *out_found = 0;
    if (out_n_granted) *out_n_granted = 0;
    if (out_holder_csid) *out_holder_csid = 0;
    if (out_holder_lkid) *out_holder_lkid = 0;
    if (out_mode) *out_mode = 0;
#ifdef SCSD_UNIT_TEST
    (void)resnam;
    return 2296u;
#else
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0)
        return 2680u; /* SS$_NOSUCHDEV -- honest, no fabricated readback */
    struct vms_register_args reg;
    memset(&reg, 0, sizeof(reg));
    if (ioctl(fd, VMS_IOCTL_REGISTER, &reg) < 0) { close(fd); return 2680u; }
    struct vms_dlm_granted_args g;
    memset(&g, 0, sizeof(g));
    if (resnam != NULL) {
        strncpy(g.resnam, resnam, sizeof(g.resnam) - 1);
        g.resnam[sizeof(g.resnam) - 1] = '\0';
    }
    if (ioctl(fd, VMS_IOCTL_DLM_GET_GRANTED, &g) < 0) { close(fd); return 2680u; }
    if (out_found) *out_found = g.found;
    if (out_n_granted) *out_n_granted = g.n_granted;
    if (out_holder_csid) *out_holder_csid = g.holder_csid;
    if (out_holder_lkid) *out_holder_lkid = g.holder_req_lkid;
    if (out_mode) *out_mode = g.granted_mode;
    uint32_t st = g.status;
    close(fd);
    return st;
#endif
}

/*
 * scsd_dlm_client_send_rebuild - node A's REMASTER LOCK REBUILD send (rd
 * vms-dca9, rung H10b). A TARGETED SEND to a peer resolved by CSID
 * (peer_by_csid), not the peer whose per-peer join-tick context drove the
 * call -- every earlier DLM client sender (scsd_dlm_send_enq,
 * scsd_dlm_client_send_op, scsd_dlm_client_send_deq_valblk) addresses the SAME
 * peer its own `ps` already belongs to; this is the first that must reach a
 * DIFFERENT peer (the resource's NEW directory master), which is why it is a
 * NEW send site rather than a reuse of one of those. Same MTYPE-10 SYSAP
 * framing, same DLM-server addressing (PS_DLM_CONID/PS_DLM_SERVER_CONID) as
 * every sibling. Carries the holder's REAL req_lkid/mode/req_csid (INV-6: never
 * a fabricated or defaulted lock). Returns 1 on send.
 */
static int scsd_dlm_client_send_rebuild(struct scsd_rx *rx, struct peer_state *ps,
                                        const char *resname, uint32_t req_lkid,
                                        uint8_t mode)
{
    if (rx == NULL || ps == NULL || ps->pb == NULL || ps->joiner_remote_conid == 0)
        return 0;

    uint32_t dlm_server = (ps->joiner_remote_conid & 0xFFFFFFF0u) |
                          OVMX_CONID_CLS_DLMSRV;

    struct scs_dlm_params p;
    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, ps_port_addr(ps), 6);
    memcpy(p.src_mac, rx->our_hw_mac, 6);
    memcpy(p.src_logical, rx->our_src_logical, 6);
    memcpy(p.peer_logical, ps_sys_addr(ps), 6);
    p.local_conid = PS_DLM_CONID(ps);
    p.remote_conid = dlm_server;
    p.recv_ack = ps->vc.seq.recv_seq;
    p.send_seq = scs_seq_advance(&ps->vc.seq);
    p.incarnation = ps->incarnation;

    struct scs_dlm_msg m;
    memset(&m, 0, sizeof(m));
    m.op = SCS_DLM_OP_REBUILD;
    m.mode = mode;
    m.req_lkid = req_lkid;
    m.req_csid = resolve_scssystemid();   /* the surviving holder's identity */
    if (resname != NULL) {
        size_t rl = strlen(resname);
        if (rl > SCS_DLM_RESNAM_MAX)
            rl = SCS_DLM_RESNAM_MAX;
        m.namelen = (uint8_t)rl;
        memcpy(m.resnam, resname, rl);
    }

    uint8_t frame[SCS_DLM_FRAME_LEN];
    if (scs_dlm_build_frame(&p, &m, frame) == 0 &&
        send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                      "OVMX$DLM REBUILD (H10b remaster)", frame,
                      sizeof(frame)) > 0)
        return 1;
    return 0;
}

/*
 * scsd_dlm_h10b_rebuild_on_depart - THE REMASTER REBUILD SEND, holder side (rd
 * vms-dca9, DLM rung H10b). Fires from the SAME class-0x04 self-departure hook
 * H10a's ingress uses (`ps` is the DEPARTING peer's own state -- the peer this
 * node's PRE-DEPARTURE HOLD was established against, scsd_dlm_h10b_hold_established
 * above), so it runs ONLY on the node that actually holds the lock (node A); on
 * a node that never sent the hold ENQ (node B), dlm_h10b_hold_established is
 * never set and this is a no-op.
 *
 *   1. Read our own origin's REAL granted mode fresh, via GETLKI on our own
 *      handle (req_lkid=1) -- never the mode we ASSUMED when we sent the ENQ.
 *   2. Re-resolve the resource's directory over the shrunk membership (the
 *      SAME LOCAL GET_RESMASTER read h10a's ingress uses) -- N, the new master.
 *   3. Look N up as a live peer (peer_by_csid) and send it a targeted
 *      SCS_DLM_OP_REBUILD carrying our REAL req_lkid/mode/req_csid.
 *
 * One-shot (dlm_h10b_rebuild_sent). Gated on OVMX_DLM_H10B; a no-op without it
 * even if a hold somehow got established (defence in depth, every other H10b
 * gate does the same).
 */
static void scsd_dlm_h10b_rebuild_on_depart(struct peer_state *ps)
{
    if (ps == NULL || !ps->dlm_h10b_hold_established || ps->dlm_h10b_rebuild_sent)
        return;
    if (getenv("OVMX_DLM_H10B") == NULL)
        return;
    ps->dlm_h10b_rebuild_sent = 1;   /* one-shot regardless of outcome below */

    const uint32_t our_lkid = 1u;   /* the SAME handle convention every DLM
                                      * rung's node-A holder uses (req_lkid=1) */
    uint32_t mode = 0;
    uint32_t gst = scsd_dlm_h10b_read_own_mode(our_lkid, &mode);

    uint32_t new_master = 0, local = 0;
    uint32_t rst = scsd_dlm_h10_read_dir(ps->dlm_h10b_resnam, &new_master, &local);

    struct scsd_rx *rx = scsd_rx_current.rx;
    struct peer_state *target = (rx != NULL) ? peer_by_csid(rx, new_master) : NULL;

    int sent = 0;
    if (rx != NULL && target != NULL && new_master != 0 && new_master != local &&
        gst == 1u) {
        sent = scsd_dlm_client_send_rebuild(rx, target, ps->dlm_h10b_resnam,
                                            our_lkid, (uint8_t)mode);
    }

    log_ts(stdout);
    printf(" SCSD-I-DLMREBUILDSENT csid=%u lkid=0x%08X mode=%s (getlki_rc=0x%08X"
           " dir_rc=0x%08X sent=%d) -- surviving holder re-registers its"
           " cross-node lock on the NEW master after the old master's departure"
           " (H10b, rd vms-dca9)\n",
           (unsigned)new_master, (unsigned)our_lkid, scs_dlm_mode_name((uint8_t)mode),
           (unsigned)gst, (unsigned)rst, sent);
    fflush(stdout);
}

/* Forward decls: the H11 contender drive (below) reuses the generic DLM client
 * sender and binds the client input routine, both defined further down. */
static int scsd_dlm_client_send_op(struct scsd_rx *rx, struct peer_state *ps,
                                   uint8_t op, uint8_t mode, uint32_t req_lkid,
                                   uint32_t master_lkid, const char *resname,
                                   const char *label);
static void scsd_dlm_cli_msg_input(struct scs_cdt *cdt, const void *msg,
                                   size_t msglen, void *ctx);

/* ==================== DLM rung H11 (rd vms-ec75): DISTRIBUTED DEADLOCK ==========
 * Edge-chasing distributed deadlock detection. See
 * docs/design-dlm-distributed-deadlock.md. The chase reads two authorities over
 * /dev/vms -- the HOME (VMS_IOCTL_DLM_ENUM_WAITS: "what does CSID H wait for?") and
 * the MASTER (VMS_IOCTL_DLM_GET_GRANTED: "who holds resource R?") -- and bounces
 * home<->master by CSID (peer_by_csid) until a probe returns to the initiator's own
 * held lock (CYCLE) or dead-ends (drop). On a cycle the GLOBAL-min victim is aborted
 * with SS$_DEADLOCK (the VICTIM leg, dispatched into the master's executive), and
 * the master completes that queued $ENQ by wiring an unprompted GRANT(SS$_DEADLOCK)
 * to the requester. Every hop reads REAL executive state (INV-6): a dropped/
 * ttl-expired probe reports no deadlock, never a fabricated cycle. */

static void scsd_ec75_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);        p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static uint32_t scsd_ec75_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * scsd_dlm_send_dlksrch - the DLKSRCH PROBE send (SEARCH-HOLDER / SEARCH-RESOURCE /
 * VICTIM forward). A TARGETED send to a peer resolved by CSID (peer_by_csid), like
 * scsd_dlm_client_send_rebuild. CHOKED (send_frame_vc) -- new in vms-ec75, listed in
 * the SEND SITE TABLE. The victim ids ride the valblk slots (design §wire). Returns
 * 1 on send.
 */
static int scsd_dlm_send_dlksrch(struct scsd_rx *rx, struct peer_state *ps,
                                 uint8_t phase, uint32_t init_csid, uint32_t init_lkid,
                                 uint32_t blocked_csid, uint32_t victim_csid,
                                 uint32_t victim_lkid, uint32_t victim_master,
                                 uint32_t ttl, const char *resnam)
{
    if (rx == NULL || ps == NULL || ps->pb == NULL || ps->joiner_remote_conid == 0)
        return 0;

    uint32_t dlm_server = (ps->joiner_remote_conid & 0xFFFFFFF0u) |
                          OVMX_CONID_CLS_DLMSRV;

    struct scs_dlm_params p;
    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, ps_port_addr(ps), 6);
    memcpy(p.src_mac, rx->our_hw_mac, 6);
    memcpy(p.src_logical, rx->our_src_logical, 6);
    memcpy(p.peer_logical, ps_sys_addr(ps), 6);
    p.local_conid = PS_DLM_CONID(ps);
    p.remote_conid = dlm_server;
    p.recv_ack = ps->vc.seq.recv_seq;
    p.send_seq = scs_seq_advance(&ps->vc.seq);
    p.incarnation = ps->incarnation;

    struct scs_dlm_msg m;
    memset(&m, 0, sizeof(m));
    m.op = SCS_DLM_OP_DLKSRCH;
    m.flags = phase;                 /* SCS_DLM_DLK_* */
    m.req_csid = init_csid;          /* initiator (cycle-close identity) */
    m.req_lkid = init_lkid;
    m.master_csid = blocked_csid;    /* SEARCH-HOLDER: Hk being chased */
    m.status = ttl;                  /* hop budget */
    scsd_ec75_put_le32(m.valblk + 0, victim_csid);   /* running MIN victim */
    scsd_ec75_put_le32(m.valblk + 4, victim_lkid);
    scsd_ec75_put_le32(m.valblk + 8, victim_master);
    if (resnam != NULL) {
        size_t rl = strlen(resnam);
        if (rl > SCS_DLM_RESNAM_MAX) rl = SCS_DLM_RESNAM_MAX;
        m.namelen = (uint8_t)rl;
        memcpy(m.resnam, resnam, rl);
    }

    uint8_t frame[SCS_DLM_FRAME_LEN];
    if (scs_dlm_build_frame(&p, &m, frame) == 0 &&
        send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb, "OVMX$DLM DLKSRCH (H11)",
                      frame, sizeof(frame)) > 0)
        return 1;
    return 0;
}

/*
 * scsd_dlm_send_victim_grant - the VICTIM SIGNAL: an unprompted SCS_DLM_OP_GRANT
 * carrying status=SS$_DEADLOCK to the victim node's DLM CLIENT, completing its
 * queued cross-node $ENQ with SS$_DEADLOCK -- exactly as the local detector's $ENQ
 * returns SS$_DEADLOCK. Structurally the H5 deferred GRANT, but the status is the
 * deadlock abort and the dest Con.ID is the requester's DLM client handle learned
 * when its WAIT $ENQ queued here (stashed on its peer_state). CHOKED (send_frame_vc)
 * -- new in vms-ec75, listed in the SEND SITE TABLE. Returns 1 on send.
 */
#define SS_DEADLOCK_VMS 3594u   /* SS$_DEADLOCK (odd=... even=error; 3594=0xE0A) */
static int scsd_dlm_send_victim_grant(struct scsd_rx *rx, struct peer_state *ps,
                                      uint32_t dest_client_conid, uint32_t victim_csid,
                                      uint32_t victim_lkid, const char *resnam)
{
    if (rx == NULL || ps == NULL || ps->pb == NULL || dest_client_conid == 0)
        return 0;

    struct scs_dlm_params p;
    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, ps_port_addr(ps), 6);
    memcpy(p.src_mac, rx->our_hw_mac, 6);
    memcpy(p.src_logical, rx->our_src_logical, 6);
    memcpy(p.peer_logical, ps_sys_addr(ps), 6);
    p.local_conid = PS_DLM_SERVER_CONID(ps);   /* from our DLM server (we mastered it) */
    p.remote_conid = dest_client_conid;        /* the victim's DLM client handle */
    p.recv_ack = ps->vc.seq.recv_seq;
    p.send_seq = scs_seq_advance(&ps->vc.seq);
    p.incarnation = ps->incarnation;

    struct scs_dlm_msg g;
    memset(&g, 0, sizeof(g));
    g.op = SCS_DLM_OP_GRANT;
    g.mode = (uint8_t)LCK$K_NLMODE;   /* NOT granted -- aborted */
    g.req_lkid = victim_lkid;         /* the victim's own request handle */
    g.status = SS_DEADLOCK_VMS;       /* SS$_DEADLOCK -- the honest abort status */
    g.req_csid = victim_csid;
    g.master_csid = resolve_scssystemid();
    if (resnam != NULL) {
        size_t rl = strlen(resnam);
        if (rl > SCS_DLM_RESNAM_MAX) rl = SCS_DLM_RESNAM_MAX;
        g.namelen = (uint8_t)rl;
        memcpy(g.resnam, resnam, rl);
    }

    uint8_t frame[SCS_DLM_FRAME_LEN];
    if (scs_dlm_build_frame(&p, &g, frame) == 0 &&
        send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                      "OVMX$DLM GRANT (H11 victim SS$_DEADLOCK)", frame,
                      sizeof(frame)) > 0)
        return 1;
    return 0;
}

/*
 * scsd_dlm_ec75_enum_waits - LOCAL VMS_IOCTL_DLM_ENUM_WAITS: the HOME authority read.
 * Returns the FIRST pending (NL) cross-node wait this node owns -- resource, master,
 * this node's own request handle + CSID. Fail-honest (all zero) with no /dev/vms.
 */
static uint32_t scsd_dlm_ec75_enum_waits(char *out_resnam, uint32_t *out_master,
                                         uint32_t *out_lkid, uint32_t *out_csid,
                                         uint32_t *out_count)
{
    if (out_resnam) out_resnam[0] = '\0';
    if (out_master) *out_master = 0;
    if (out_lkid) *out_lkid = 0;
    if (out_csid) *out_csid = 0;
    if (out_count) *out_count = 0;
#ifdef SCSD_UNIT_TEST
    return 2296u;
#else
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0)
        return 2680u;
    struct vms_register_args reg;
    memset(&reg, 0, sizeof(reg));
    if (ioctl(fd, VMS_IOCTL_REGISTER, &reg) < 0) { close(fd); return 2680u; }
    struct vms_dlm_enum_waits_args a;
    memset(&a, 0, sizeof(a));
    if (ioctl(fd, VMS_IOCTL_DLM_ENUM_WAITS, &a) < 0) { close(fd); return 2680u; }
    if (out_count) *out_count = a.count;
    if (a.count > 0) {
        if (out_resnam) { memcpy(out_resnam, a.ent[0].resnam, 31); out_resnam[31] = '\0'; }
        if (out_master) *out_master = a.ent[0].master_csid;
        if (out_lkid) *out_lkid = a.ent[0].req_lkid;
        if (out_csid) *out_csid = a.ent[0].req_csid;
    }
    uint32_t st = a.status;
    close(fd);
    return st;
#endif
}

/*
 * scsd_dlm_ec75_abort_victim - LOCAL DLKSRCH VICTIM dispatch: this node masters the
 * victim's queued request; abort it (remove from res->waiting, complete with
 * SS$_DEADLOCK). Idempotent. Returns the executive status (SS$_DEADLOCK when a
 * waiter was aborted this call, SS$_NORMAL when none matched -- already gone).
 */
static uint32_t scsd_dlm_ec75_abort_victim(uint32_t victim_csid, uint32_t victim_lkid,
                                           uint32_t *out_aborted)
{
    if (out_aborted) *out_aborted = 0;
#ifdef SCSD_UNIT_TEST
    (void)victim_csid; (void)victim_lkid;
    return 2296u;
#else
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0)
        return 2680u;
    struct vms_register_args reg;
    memset(&reg, 0, sizeof(reg));
    if (ioctl(fd, VMS_IOCTL_REGISTER, &reg) < 0) { close(fd); return 2680u; }
    struct vms_dlm_xnode_args args;
    memset(&args, 0, sizeof(args));
    args.op = VMS_DLM_OP_DLKSRCH;
    args.flags = VMS_DLM_DLK_VICTIM;
    args.req_csid = victim_csid;
    args.req_lkid = victim_lkid;
    if (ioctl(fd, VMS_IOCTL_DLM_XNODE, &args) < 0) { close(fd); return 2680u; }
    if (out_aborted) *out_aborted = args.queued;   /* 1 iff a waiter was aborted now */
    uint32_t st = args.status;
    close(fd);
    return st;
#endif
}

/* Forward: the SEARCH-RESOURCE processing (also reached by a SEARCH-HOLDER whose
 * waited resource this node itself masters -- the home==master collapse). */
static void scsd_dlm_ec75_process_resource(struct scsd_rx *rx, const char *resnam,
                                           uint32_t init_csid, uint32_t init_lkid,
                                           uint32_t victim_csid, uint32_t victim_lkid,
                                           uint32_t victim_master, uint32_t ttl);

/*
 * scsd_dlm_ec75_deliver_victim - the cycle is confirmed; abort the GLOBAL-min victim.
 * If THIS node masters the victim's request (victim_master == us), abort it in the
 * executive and wire the GRANT(SS$_DEADLOCK) to the requester. Otherwise forward a
 * DLKSRCH(VICTIM) to the victim's master (the multi-hop case).
 */
static void scsd_dlm_ec75_deliver_victim(struct scsd_rx *rx, uint32_t init_csid,
                                         uint32_t init_lkid, uint32_t victim_csid,
                                         uint32_t victim_lkid, uint32_t victim_master,
                                         const char *resnam)
{
    uint32_t us = resolve_scssystemid();
    if (victim_master == us) {
        uint32_t aborted = 0;
        uint32_t st = scsd_dlm_ec75_abort_victim(victim_csid, victim_lkid, &aborted);
        struct peer_state *vp = peer_by_csid(rx, victim_csid);
        uint32_t dest = vp ? vp->dlm_ec75_init_conid : 0;
        const char *vres = (vp && vp->dlm_ec75_init_resnam[0]) ?
                           vp->dlm_ec75_init_resnam : resnam;
        int sent = (vp != NULL) ?
            scsd_dlm_send_victim_grant(rx, vp, dest, victim_csid, victim_lkid, vres) : 0;
        log_ts(stdout);
        printf(" SCSD-I-DLKVICTIM, victim=(csid=%u,lkid=0x%08X) ABORTED with"
               " SS$_DEADLOCK (abort_rc=0x%08X aborted=%u); GRANT(SS$_DEADLOCK)"
               " %s to the victim's $ENQ -- the cross-node cycle is broken by"
               " exactly one victim (H11, rd vms-ec75)\n",
               (unsigned)victim_csid, (unsigned)victim_lkid, (unsigned)st,
               (unsigned)aborted, sent ? "WIRED" : "NOT wired");
        fflush(stdout);
    } else {
        struct peer_state *mp = peer_by_csid(rx, victim_master);
        int sent = (mp != NULL) ?
            scsd_dlm_send_dlksrch(rx, mp, SCS_DLM_DLK_VICTIM, init_csid, init_lkid, 0,
                                  victim_csid, victim_lkid, victim_master, 1, resnam) : 0;
        log_ts(stdout);
        printf(" SCSD-I-DLKVICTIM, forwarding VICTIM to master CSID=%u for"
               " victim=(csid=%u,lkid=0x%08X) sent=%d (H11)\n",
               (unsigned)victim_master, (unsigned)victim_csid,
               (unsigned)victim_lkid, sent);
        fflush(stdout);
    }
}

static void scsd_dlm_ec75_process_resource(struct scsd_rx *rx, const char *resnam,
                                           uint32_t init_csid, uint32_t init_lkid,
                                           uint32_t victim_csid, uint32_t victim_lkid,
                                           uint32_t victim_master, uint32_t ttl)
{
    uint32_t found = 0, n = 0, holder_csid = 0, holder_lkid = 0, mode = 0;
    uint32_t rc = scsd_dlm_h10b_get_granted(resnam, &found, &n, &holder_csid,
                                            &holder_lkid, &mode);
    log_ts(stdout);
    printf(" SCSD-I-DLKSRCH, SEARCH-RESOURCE resnam='%s' -> master read holder"
           " csid=%u lkid=0x%08X (found=%u get_granted_rc=0x%08X) initiator=%u"
           " ttl=%u (H11)\n",
           resnam ? resnam : "", (unsigned)holder_csid, (unsigned)holder_lkid,
           (unsigned)found, (unsigned)rc, (unsigned)init_csid, (unsigned)ttl);
    fflush(stdout);

    if (!found || holder_csid == 0)
        return;   /* dead end -- resource unheld; no cycle on this path (drop) */

    if (holder_csid == init_csid) {
        /* The chase returned to the initiator's OWN held lock -- CYCLE. */
        log_ts(stdout);
        printf(" SCSD-I-DLKCYCLE, DISTRIBUTED DEADLOCK DETECTED: probe from"
               " initiator CSID=%u closed on holder CSID=%u of resnam='%s';"
               " global-min victim=(csid=%u,lkid=0x%08X) -- a REAL cross-node"
               " wait-for cycle read off res->granted + pending origins (H11,"
               " rd vms-ec75)\n",
               (unsigned)init_csid, (unsigned)holder_csid, resnam ? resnam : "",
               (unsigned)victim_csid, (unsigned)victim_lkid);
        fflush(stdout);
        scsd_dlm_ec75_deliver_victim(rx, init_csid, init_lkid, victim_csid,
                                     victim_lkid, victim_master, resnam);
        return;
    }

    /* Not the initiator -- chase the next holder: forward SEARCH-HOLDER. */
    if (ttl == 0)
        return;
    struct peer_state *hp = peer_by_csid(rx, holder_csid);
    if (hp != NULL) {
        scsd_dlm_send_dlksrch(rx, hp, SCS_DLM_DLK_SEARCH_HOLDER, init_csid, init_lkid,
                              holder_csid, victim_csid, victim_lkid, victim_master,
                              ttl - 1, resnam);
    }
}

/*
 * scsd_dlm_ec75_handle_search - orchestrate a received DLKSRCH SEARCH leg. Reads the
 * two authorities and forwards / detects / drops. Delegates every SEND to
 * scsd_dlm_send_dlksrch / scsd_dlm_send_victim_grant, so this function itself never
 * calls send_frame_vc (it is not a SEND SITE).
 */
static void scsd_dlm_ec75_handle_search(struct scsd_rx *rx, const struct scs_dlm_view *v)
{
    uint8_t phase = (uint8_t)v->msg.flags;
    uint32_t init_csid = v->msg.req_csid;
    uint32_t init_lkid = v->msg.req_lkid;
    uint32_t ttl = v->msg.status;
    uint32_t victim_csid = scsd_ec75_get_le32(v->msg.valblk + 0);
    uint32_t victim_lkid = scsd_ec75_get_le32(v->msg.valblk + 4);
    uint32_t victim_master = scsd_ec75_get_le32(v->msg.valblk + 8);
    char resnam[32];
    size_t rl = v->msg.namelen; if (rl > 31) rl = 31;
    memcpy(resnam, v->msg.resnam, rl); resnam[rl] = '\0';
    uint32_t us = resolve_scssystemid();

    if (ttl == 0) {
        log_ts(stdout);
        printf(" SCSD-I-DLKSRCH, probe ttl EXPIRED (initiator=%u) -- bounded search,"
               " no deadlock found (drop, H11)\n", (unsigned)init_csid);
        fflush(stdout);
        return;
    }

    if (phase == SCS_DLM_DLK_VICTIM) {
        /* We MASTER the victim's request (multi-hop victim routing). */
        scsd_dlm_ec75_deliver_victim(rx, init_csid, init_lkid, victim_csid,
                                     victim_lkid, victim_master, resnam);
        return;
    }

    if (phase == SCS_DLM_DLK_SEARCH_HOLDER) {
        /* We are Hk: enumerate OUR pending waits (HOME authority). */
        char wres[32]; uint32_t wmaster = 0, wlkid = 0, wcsid = 0, wcount = 0;
        uint32_t rc = scsd_dlm_ec75_enum_waits(wres, &wmaster, &wlkid, &wcsid, &wcount);
        log_ts(stdout);
        printf(" SCSD-I-DLKSRCH, SEARCH-HOLDER at CSID=%u: pending waits=%u"
               " first='%s' master=%u lkid=0x%08X (enum_rc=0x%08X) initiator=%u"
               " ttl=%u (H11)\n",
               (unsigned)us, (unsigned)wcount, wcount ? wres : "", (unsigned)wmaster,
               (unsigned)wlkid, (unsigned)rc, (unsigned)init_csid, (unsigned)ttl);
        fflush(stdout);
        if (wcount == 0)
            return;   /* not blocked -- dead end on this path (drop) */
        /* Update the running GLOBAL-min victim with OUR waiting request's id. */
        if (wcsid < victim_csid || (wcsid == victim_csid && wlkid < victim_lkid)) {
            victim_csid = wcsid; victim_lkid = wlkid; victim_master = wmaster;
        }
        if (wmaster == us) {
            /* We also master the waited resource -- collapse the hop locally. */
            scsd_dlm_ec75_process_resource(rx, wres, init_csid, init_lkid,
                                           victim_csid, victim_lkid, victim_master,
                                           ttl - 1);
        } else {
            struct peer_state *mp = peer_by_csid(rx, wmaster);
            if (mp != NULL)
                scsd_dlm_send_dlksrch(rx, mp, SCS_DLM_DLK_SEARCH_RESOURCE, init_csid,
                                      init_lkid, 0, victim_csid, victim_lkid,
                                      victim_master, ttl - 1, wres);
        }
        return;
    }

    if (phase == SCS_DLM_DLK_SEARCH_RESOURCE) {
        /* We master resnam -- who holds it? (MASTER authority). */
        scsd_dlm_ec75_process_resource(rx, resnam, init_csid, init_lkid,
                                       victim_csid, victim_lkid, victim_master, ttl);
        return;
    }
}

/*
 * scsd_dlm_ec75_drive - a CONTENDER's timed hold-then-wait sequence to the master C.
 * On the DLM VC to C being ready, phase 1 (immediately): $ENQ the HOLD resource EX
 * (req_lkid 1). Phase 2 (after a settle window, so BOTH contenders' holds are
 * established at C before either WAIT arrives): $ENQ the WAIT resource EX
 * (req_lkid 2), which QUEUES behind the other contender -- the cross-node wait-for
 * edge. The settle window is the only timing dependence and is generous; the cycle
 * detection itself is event-driven off the queued-reply. Gated on OVMX_DLM_EC75 +
 * the target CSID (OVMX_DLM_EC75_MASTER); a no-op on the master node.
 */
#define OVMX_EC75_WAIT_SETTLE_MS 6000u
static void scsd_dlm_ec75_drive(struct scsd_rx *rx, struct peer_state *ps, uint64_t now_ms)
{
    if (rx == NULL || ps == NULL || ps->pb == NULL || !scsd_member_initiate_enabled())
        return;
    if (getenv("OVMX_DLM_EC75") == NULL)
        return;
    /* Mark the H11 role armed on this peer, so the client input's contender-side
     * handling (queued-reply -> pending origin, victim GRANT -> SS$_DEADLOCK) fires
     * on the replies this drive's sends elicit. */
    ps->dlm_ec75 = 1;
    if (!ps->joiner_connected || ps->joiner_remote_conid == 0)
        return;

    const char *hold = getenv("OVMX_DLM_EC75_HOLD");
    const char *wait = getenv("OVMX_DLM_EC75_WAIT");
    const char *master_env = getenv("OVMX_DLM_EC75_MASTER");
    if (hold == NULL || wait == NULL || master_env == NULL)
        return;
    uint32_t master_csid = (uint32_t)strtoul(master_env, NULL, 10);
    if ((uint32_t)peer_node_number(ps) != master_csid)
        return;   /* only drive toward the master node C */

    /* Make sure our DLM client CDT is bound (mirrors scsd_dlm_send_enq). */
    struct scs_cdt *cli = scs_cdl_lookup(&scsd_cdl, PS_DLM_CONID(ps));
    if (cli == NULL)
        cli = scs_cdl_alloc_conid(&scsd_cdl, PS_DLM_CONID(ps), "OVMX$DLM",
                                  "OVMX$DLM", ps->pb);
    if (cli != NULL)
        scs_cdt_set_handlers(cli, scsd_dlm_cli_msg_input, NULL, NULL, ps);

    if (ps->dlm_ec75_join_ms == 0)
        ps->dlm_ec75_join_ms = now_ms;

    /* Phase 1: HOLD (req_lkid 1). */
    if (!ps->dlm_ec75_hold_sent) {
        if (scsd_dlm_client_send_op(rx, ps, SCS_DLM_OP_ENQ, LCK$K_EXMODE, 1u, 0,
                                    hold, "OVMX$DLM ENQ HOLD (H11)")) {
            ps->dlm_ec75_hold_sent = 1;
            log_ts(stdout);
            printf(" SCSD-I-DLKENQ, sent HOLD $ENQ EX resnam='%s' (req_lkid=1) to"
                   " master CSID=%u -- our end of the cross-node cycle (H11)\n",
                   hold, (unsigned)master_csid);
            fflush(stdout);
        }
        return;
    }

    /* Phase 2: WAIT (req_lkid 2), after the settle window. The window is
     * env-tunable per node (OVMX_DLM_EC75_WAIT_MS) so the harness can STAGGER the
     * two contenders: the designated initiator waits LONGER, so by the time its
     * WAIT queues (and the master initiates the search) the OTHER contender's WAIT
     * has already queued and its PENDING origin exists -- else a SEARCH-HOLDER
     * probe could reach a node before it recorded the wait-for edge and dead-end
     * on a false negative. Both windows exceed the (sub-second) hold-grant time. */
    uint64_t settle = OVMX_EC75_WAIT_SETTLE_MS;
    const char *wms = getenv("OVMX_DLM_EC75_WAIT_MS");
    if (wms != NULL && wms[0] != '\0')
        settle = (uint64_t)strtoul(wms, NULL, 10);
    if (!ps->dlm_ec75_wait_sent &&
        now_ms - ps->dlm_ec75_join_ms >= settle) {
        if (scsd_dlm_client_send_op(rx, ps, SCS_DLM_OP_ENQ, LCK$K_EXMODE, 2u, 0,
                                    wait, "OVMX$DLM ENQ WAIT (H11)")) {
            ps->dlm_ec75_wait_sent = 1;
            log_ts(stdout);
            printf(" SCSD-I-DLKENQ, sent WAIT $ENQ EX resnam='%s' (req_lkid=2) to"
                   " master CSID=%u -- expect it to QUEUE behind the peer holder;"
                   " the master then initiates the deadlock search (H11)\n",
                   wait, (unsigned)master_csid);
            fflush(stdout);
        }
    }
}

/*
 * scsd_dlm_srv_msg_input - the DLM server CDT's message input routine. Reads the
 * received frame from scsd_rx_current (the same OVMX design choice
 * scsd_mscp_srv_msg_input uses), decodes it with scs_dlm_parse, and dispatches
 * the decoded request to the executive. Rung 1 records the outcome; rung 2 turns
 * a GRANT response into an outbound reply from here.
 */
static void scsd_dlm_srv_msg_input(struct scs_cdt *cdt, const void *msg,
                                   size_t msglen, void *ctx)
{
    (void)cdt;
    (void)msg;
    (void)msglen;
    struct peer_state *ps = (struct peer_state *)ctx;
    struct scsd_rx_frame cur = scsd_rx_current;
    struct scs_dlm_view v;

    if (ps == NULL || cur.frame == NULL ||
        scs_dlm_parse(cur.frame, (size_t)cur.len, &v) != 0) {
        return; /* not a well-formed DLM-over-SCS frame -- nothing to dispatch */
    }

    /* THE REMASTER LOCK REBUILD, NEW-MASTER SIDE (rd vms-dca9, rung H10b). A
     * surviving holder re-registers its cross-node lock here after THIS node
     * became the resource's new directory master (its old master departed).
     * Dispatch op=REBUILD into OUR executive -- vms_lock_dlm_xnode_rebuild
     * reconstructs the holder's REAL prior grant DIRECTLY into res->granted,
     * never re-deriving it through the enqueue/grant core (INV-6) -- then read
     * the rebuilt lock BACK via VMS_IOCTL_DLM_GET_GRANTED so the harness can
     * VALUE-VERIFY it equals what the holder sent. Distinct from the ENQ/DEQ
     * request/response leg below: REBUILD gets NO wire reply (the holder
     * already holds the lock; it is not waiting on one). Gated on
     * OVMX_DLM_H10B so a node without it armed is byte-identical to before
     * this rung existed. */
    if (v.msg.op == SCS_DLM_OP_REBUILD) {
        if (getenv("OVMX_DLM_H10B") == NULL)
            return;
        uint32_t status = scsd_dlm_dispatch_to_executive(&v.msg, NULL, NULL, NULL);
        log_ts(stdout);
        printf(" SCSD-I-DLMRX, cross-node REBUILD from CSID=%u resnam='%.*s'"
               " -> executive status=0x%08X\n",
               (unsigned)v.msg.req_csid, (int)v.msg.namelen, v.msg.resnam,
               (unsigned)status);
        fflush(stdout);

        uint32_t found = 0, n_granted = 0, holder_csid = 0, holder_lkid = 0, mode = 0;
        uint32_t rst = scsd_dlm_h10b_get_granted((const char *)v.msg.resnam, &found,
                                                  &n_granted, &holder_csid,
                                                  &holder_lkid, &mode);
        log_ts(stdout);
        printf(" SCSD-I-DLMREBUILT found=%u n_granted=%u holder_csid=%u lkid=0x%08X"
               " mode=%s (rebuild_rc=0x%08X readback_rc=0x%08X) -- this node is"
               " now the resource's master; the surviving holder's lock was"
               " RECONSTRUCTED from its REAL origin state, read back from"
               " res->granted (H10b, rd vms-dca9)\n",
               (unsigned)found, (unsigned)n_granted, (unsigned)holder_csid,
               (unsigned)holder_lkid, scs_dlm_mode_name((uint8_t)mode),
               (unsigned)status, (unsigned)rst);
        fflush(stdout);
        return;
    }

    /* DISTRIBUTED DEADLOCK SEARCH (rd vms-ec75, rung H11). A DLKSRCH probe -- a
     * SEARCH-HOLDER / SEARCH-RESOURCE / VICTIM leg -- rides the DLM SYSAP to this
     * node's server handle. Orchestrate it over the two readback authorities; every
     * hop reads REAL executive state (INV-6). Gated on OVMX_DLM_EC75 so a node
     * without it armed is byte-identical to before this rung. */
    if (v.msg.op == SCS_DLM_OP_DLKSRCH) {
        if (getenv("OVMX_DLM_EC75") == NULL || !scsd_member_initiate_enabled())
            return;
        log_ts(stdout);
        printf(" SCSD-I-DLKSRCH, RECEIVED probe phase=%u from CSID=%u initiator=%u"
               " ttl=%u resnam='%.*s' (H11)\n",
               (unsigned)v.msg.flags, (unsigned)peer_node_number(ps),
               (unsigned)v.msg.req_csid, (unsigned)v.msg.status,
               (int)v.msg.namelen, v.msg.resnam);
        fflush(stdout);
        scsd_dlm_ec75_handle_search(scsd_rx_current.rx, &v);
        return;
    }

    /* Only a REQUEST (ENQ/DEQ) is dispatched + answered here. A GRANT/BLKAST is
     * a response class and belongs to the client input, not this server input;
     * ignoring it also makes a stray GRANT unable to trigger a reply storm. */
    if (v.msg.op != SCS_DLM_OP_ENQ && v.msg.op != SCS_DLM_OP_DEQ) {
        return;
    }

    /* Hand the decoded request to the executive's cross-node lock handler over
     * /dev/vms (rung 1). REAL status: SS$_UNSUPPORTED on a live /dev/vms (the
     * rung-1 dispatch stub grants nothing), SS$_NOSUCHDEV when no executive is
     * present (the Docker harness -- Rule 9: Docker is not a runtime, there is
     * no /dev/vms). Either way it is HONEST -- never a fabricated grant. */
    struct scsd_dlm_held held;
    struct scsd_dlm_defer defer;
    uint32_t status = scsd_dlm_dispatch_to_executive(&v.msg, &held, &defer, NULL);
    rx_dlm_dispatched++;
    log_ts(stdout);
    printf(" SCSD-I-DLMRX, cross-node %s from CSID=%u resnam='%.*s'"
           " -> executive status=0x%08X%s\n",
           scs_dlm_op_name(v.msg.op), (unsigned)v.msg.req_csid,
           (int)v.msg.namelen, v.msg.resnam, (unsigned)status,
           status == 1u ? " (GRANTED)" : "");
    fflush(stdout);

    /* DLM rung 2 (vms-e8f1): the HELD-LOCK PROOF. On a GRANTED cross-node $ENQ,
     * print what the master's OWN resource DB (GET_RESMASTER) reports -- the
     * resource is mastered HERE and a lock is genuinely held FOR the remote
     * requester's CSID. This is the machine-checkable "B holds a real lock for
     * A's CSID" the H4 verdict reads; it is a READ of executive state, never a
     * fabricated line (INV-6). */
    if (held.queried) {
        log_ts(stdout);
        printf(" SCSD-I-DLMHELD, master DB: resnam='%.*s' found=%u"
               " is_local_master=%u n_granted=%u held_for_csid=%u"
               " master_csid=%u\n",
               (int)v.msg.namelen, v.msg.resnam, (unsigned)held.found,
               (unsigned)held.is_local_master, (unsigned)held.n_granted,
               (unsigned)held.held_for_csid, (unsigned)held.master_csid);
        fflush(stdout);
    }

    /* vms-164d (DLM rung-1b): THE B->A RESPONSE LEG. The executive's real status
     * travels back to the requester as an SCS_DLM_OP_GRANT frame on the same VC.
     * INV-6: this GRANTS NOTHING -- it carries `status` verbatim (SS$_UNSUPPORTED
     * / SS$_NOSUCHDEV), never a fabricated success. Gated on the member-role flag
     * so flag-off is byte-identical (a real VAX drives its own DLM; OVMX does not
     * originate cross-node DLM traffic without the flag). */
    struct scsd_rx *rx = scsd_rx_current.rx;
    if (rx == NULL || !scsd_member_initiate_enabled()) {
        return;
    }
    struct scs_dlm_params gp;
    memset(&gp, 0, sizeof(gp));
    memcpy(gp.dst_mac, ps_port_addr(ps), 6);
    memcpy(gp.src_mac, rx->our_hw_mac, 6);
    memcpy(gp.src_logical, rx->our_src_logical, 6);
    memcpy(gp.peer_logical, ps_sys_addr(ps), 6);
    gp.local_conid = PS_DLM_SERVER_CONID(ps);
    gp.remote_conid = v.local_conid;              /* the requester's DLM client handle */
    gp.recv_ack = ps->vc.seq.recv_seq;
    gp.send_seq = scs_seq_advance(&ps->vc.seq);
    gp.incarnation = ps->incarnation;

    struct scs_dlm_msg g;
    memset(&g, 0, sizeof(g));
    g.op = SCS_DLM_OP_GRANT;
    /* The granted mode the requester must record (vms-6ca, H5): a GRANT (status
     * SS$_NORMAL) carries the granted mode; a QUEUED-reply (status
     * VMS_DLM_STS_QUEUED == 0) carries NL so the requester's origin record stays
     * genuinely PENDING -- never a fabricated grant (INV-6). */
    g.mode = (status == 1u) ? v.msg.mode : (uint8_t)LCK$K_NLMODE;
    g.req_lkid = v.msg.req_lkid;
    g.master_lkid = held.master_lkid;             /* the master's handle (H5): lets
                                                   * the requester $DEQ the holder */
    g.status = status;                            /* the HONEST executive status */
    g.req_csid = v.msg.req_csid;
    g.master_csid = resolve_scssystemid();        /* this node mastered the request */
    g.namelen = v.msg.namelen;
    memcpy(g.resnam, v.msg.resnam, sizeof(g.resnam));
    /* LVB read crossing (rd vms-eeb, rung H9): the GRANT reply carries the
     * master's current value block back to the requester, so its cross-node
     * $ENQ reads the master's LVB. held.valblk is the executive's real
     * res->valblk (read above), never a fabricated block (INV-6). */
    memcpy(g.valblk, held.valblk, sizeof(g.valblk));

    uint8_t frame[SCS_DLM_FRAME_LEN];
    if (scs_dlm_build_frame(&gp, &g, frame) == 0 &&
        send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                      "OVMX$DLM GRANT (rung-1b response)", frame,
                      sizeof(frame)) > 0) {
        ps->dlm_srv_responses++;
        log_ts(stdout);
        printf(" SCSD-I-DLMGRANT, sent GRANT status=0x%08X back to CSID=%u"
               " (local=0x%08X remote=0x%08X) -- %s\n",
               (unsigned)status, (unsigned)v.msg.req_csid,
               (unsigned)PS_DLM_SERVER_CONID(ps), (unsigned)v.local_conid,
               status == 1u ? "cross-node lock GRANTED (held for requester)"
                            : "honest response, request not granted");
        fflush(stdout);
    }

    /* THE BLKAST WIRE (vms-76d, DLM epic vms-7fa rung H6). If THIS $ENQ QUEUED
     * behind a cross-node HOLDER, the executive named that holder (held.blocking_*)
     * -- it must receive a blocking AST. WIRE a real SCS_DLM_OP_BLKAST to it,
     * naming the holder's OWN (requester-side) lock handle so the holder node finds
     * its ORIGIN record and fires the AST. This is the master genuinely delivering
     * the blocking-AST decision over the wire (INV-6: held.blocking_* is a READ of
     * the executive's directive, never fabricated). Same-peer scope, exactly as the
     * deferred GRANT below: the holder (#1) and the contender (#2) share this VC. */
    if (held.blocking_req_lkid != 0) {
        struct scs_dlm_params bp;
        memset(&bp, 0, sizeof(bp));
        memcpy(bp.dst_mac, ps_port_addr(ps), 6);
        memcpy(bp.src_mac, rx->our_hw_mac, 6);
        memcpy(bp.src_logical, rx->our_src_logical, 6);
        memcpy(bp.peer_logical, ps_sys_addr(ps), 6);
        bp.local_conid = PS_DLM_SERVER_CONID(ps);
        bp.remote_conid = v.local_conid;          /* the holder's DLM client handle */
        bp.recv_ack = ps->vc.seq.recv_seq;
        bp.send_seq = scs_seq_advance(&ps->vc.seq);
        bp.incarnation = ps->incarnation;

        struct scs_dlm_msg bm;
        memset(&bm, 0, sizeof(bm));
        bm.op = SCS_DLM_OP_BLKAST;
        bm.mode = v.msg.mode;                     /* the conflicting requested mode */
        bm.req_lkid = held.blocking_req_lkid;     /* the HOLDER's own handle (target) */
        bm.master_lkid = held.master_lkid;
        bm.status = 1u;
        bm.req_csid = held.blocking_csid;         /* the holder to notify */
        bm.master_csid = resolve_scssystemid();
        bm.namelen = v.msg.namelen;
        memcpy(bm.resnam, v.msg.resnam, sizeof(bm.resnam));

        uint8_t bframe[SCS_DLM_FRAME_LEN];
        if (scs_dlm_build_frame(&bp, &bm, bframe) == 0 &&
            send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                          "OVMX$DLM BLKAST (H6)", bframe, sizeof(bframe)) > 0) {
            ps->dlm_srv_responses++;
            ps->dlm_blksent = 1;
            log_ts(stdout);
            printf(" SCSD-I-DLMBLKSENT, WIRED BLKAST to holder req_lkid=0x%08X"
                   " CSID=%u -- a queued cross-node request blocks this holder; the"
                   " master fires its blocking AST over SCS (H6)\n",
                   (unsigned)held.blocking_req_lkid, (unsigned)held.blocking_csid);
            fflush(stdout);
        }
    }

    /* INITIATE THE DISTRIBUTED DEADLOCK SEARCH (rd vms-ec75, rung H11). This ENQ
     * QUEUED behind a cross-node HOLDER (held.blocking_csid != 0) -- exactly the
     * first wait-for edge the search chases. When THIS node masters the queued
     * request and it belongs to the designated INITIATOR (OVMX_DLM_EC75_INIT), send
     * one SEARCH-HOLDER probe to the blocking holder's node. Single-initiator by
     * design (the minimal faithful slice): only the initiator's queued wait fires a
     * search, though the global-min victim rule the search ships is correct under
     * concurrent bidirectional initiation too. Stash the requester's DLM client
     * Con.ID + waited resource so the eventual victim GRANT can reach it. */
    if (getenv("OVMX_DLM_EC75") != NULL && held.blocking_csid != 0 &&
        held.blocking_req_lkid != 0) {
        ps->dlm_ec75 = 1;   /* mark the master role armed for the client input, too */
        const char *init_env = getenv("OVMX_DLM_EC75_INIT");
        uint32_t init_csid = init_env ? (uint32_t)strtoul(init_env, NULL, 10) : 0;
        if (init_csid != 0 && v.msg.req_csid == init_csid && !ps->dlm_ec75_search_sent) {
            /* Remember how to reach the initiator's $ENQ for the victim GRANT. */
            ps->dlm_ec75_init_conid = v.local_conid;
            ps->dlm_ec75_init_csid = v.msg.req_csid;
            {
                size_t rl = v.msg.namelen;
                if (rl > sizeof(ps->dlm_ec75_init_resnam) - 1)
                    rl = sizeof(ps->dlm_ec75_init_resnam) - 1;
                memcpy(ps->dlm_ec75_init_resnam, v.msg.resnam, rl);
                ps->dlm_ec75_init_resnam[rl] = '\0';
            }
            struct peer_state *hp = peer_by_csid(scsd_rx_current.rx, held.blocking_csid);
            if (hp != NULL &&
                scsd_dlm_send_dlksrch(scsd_rx_current.rx, hp,
                                      SCS_DLM_DLK_SEARCH_HOLDER,
                                      v.msg.req_csid, v.msg.req_lkid,
                                      held.blocking_csid,
                                      /* seed victim = the initiator's own request */
                                      v.msg.req_csid, v.msg.req_lkid,
                                      resolve_scssystemid(),
                                      16u /* ttl = MAX_DEADLOCK_DEPTH */,
                                      (const char *)v.msg.resnam)) {
                ps->dlm_ec75_search_sent = 1;
                log_ts(stdout);
                printf(" SCSD-I-DLKSRCH, INITIATED search: initiator=(csid=%u,"
                       " lkid=0x%08X) waits '%.*s' behind holder CSID=%u; chasing the"
                       " cross-node wait-for edge (H11, rd vms-ec75)\n",
                       (unsigned)v.msg.req_csid, (unsigned)v.msg.req_lkid,
                       (int)v.msg.namelen, v.msg.resnam,
                       (unsigned)held.blocking_csid);
                fflush(stdout);
            }
        }
    }

    /* THE DEFERRED GRANT (vms-6ca, DLM epic vms-7fa rung H5). If THIS $DEQ
     * released a holder and thereby GRANTED a previously-QUEUED cross-node
     * request, the executive named that request in `defer`. WIRE a deferred
     * SCS_DLM_OP_GRANT (status SS$_NORMAL, the granted mode) to the requester so
     * ITS origin record flips from pending to granted -- the block-then-grant
     * completion, carried over the live SCS wire, driven by a real release. This
     * is the reply the master pushes UNPROMPTED (not in answer to a message the
     * requester just sent). INV-6: it carries a grant the executive genuinely
     * made (`defer` is a READ of the DEQ report), never a fabricated success.
     *
     * H5 proves the SAME-PEER case (the released holder and the queued waiter are
     * the same peer, so the deferred GRANT rides this peer's VC); routing a
     * deferred grant to a DIFFERENT node by CSID is a later multi-peer rung. */
    if (defer.flipped && defer.req_lkid != 0) {
        struct scs_dlm_params dp;
        memset(&dp, 0, sizeof(dp));
        memcpy(dp.dst_mac, ps_port_addr(ps), 6);
        memcpy(dp.src_mac, rx->our_hw_mac, 6);
        memcpy(dp.src_logical, rx->our_src_logical, 6);
        memcpy(dp.peer_logical, ps_sys_addr(ps), 6);
        dp.local_conid = PS_DLM_SERVER_CONID(ps);
        dp.remote_conid = v.local_conid;          /* the requester's DLM client handle */
        dp.recv_ack = ps->vc.seq.recv_seq;
        dp.send_seq = scs_seq_advance(&ps->vc.seq);
        dp.incarnation = ps->incarnation;

        struct scs_dlm_msg dg;
        memset(&dg, 0, sizeof(dg));
        dg.op = SCS_DLM_OP_GRANT;
        dg.mode = (uint8_t)defer.mode;            /* the mode it was granted at (EX) */
        dg.req_lkid = defer.req_lkid;             /* the queued request's own handle */
        dg.master_lkid = defer.master_lkid;
        dg.status = 1u;                           /* SS$_NORMAL -- a REAL grant     */
        dg.req_csid = defer.req_csid;
        dg.master_csid = resolve_scssystemid();
        dg.namelen = v.msg.namelen;
        memcpy(dg.resnam, v.msg.resnam, sizeof(dg.resnam));

        uint8_t dframe[SCS_DLM_FRAME_LEN];
        if (scs_dlm_build_frame(&dp, &dg, dframe) == 0 &&
            send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                          "OVMX$DLM deferred GRANT (H5)", dframe,
                          sizeof(dframe)) > 0) {
            ps->dlm_srv_responses++;
            log_ts(stdout);
            printf(" SCSD-I-DLMDEFER, WIRED deferred GRANT req_lkid=0x%08X mode=%s"
                   " to CSID=%u -- a queued cross-node request GRANTED by the real"
                   " release (block-then-grant over SCS)\n",
                   (unsigned)defer.req_lkid, scs_dlm_mode_name((uint8_t)defer.mode),
                   (unsigned)defer.req_csid);
            fflush(stdout);
        }
    }

    /* THE LVB WIRE, READER SIDE (rd vms-d81, H8). This node MASTERs the resource;
     * a remote holder's cross-node $DEQ carrying LCK_M_VALBLK just replicated its
     * value block into res->valblk (the executive dispatch above ran
     * vms_lock_dlm_xnode_deq). Prove the value landed by reading it back with a
     * LOCAL $ENQ on THIS node and printing it verbatim. Gated on OVMX_DLM_H8 and a
     * $DEQ that actually carried the flag; independent of the wire-response gate
     * (a local read originates no cross-node traffic). INV-6: the marker prints
     * the REAL bytes the executive returned -- if the wire write never happened,
     * this reads zeros and the host verdict FAILS on the mismatch, never passes. */
    if (getenv("OVMX_DLM_H8") != NULL && v.msg.op == SCS_DLM_OP_DEQ &&
        (v.msg.flags & LCK_M_VALBLK) && !ps->dlm_h8_read_done) {
        ps->dlm_h8_read_done = 1;
        uint8_t rdblk[LCK$C_VALBLK_LEN];
        uint32_t rst = scsd_dlm_local_read_valblk((const char *)v.msg.resnam, rdblk);
        char hx[33];
        scsd_hex16(rdblk, hx);
        log_ts(stdout);
        printf(" SCSD-I-DLMLVBRD, name=%.*s val=%s -- LOCAL $ENQ on the master read"
               " the value block a remote holder wrote over SCS (read rc=0x%08X);"
               " the LVB replicated A->B (H8)\n",
               (int)v.msg.namelen, v.msg.resnam, hx, (unsigned)rst);
        fflush(stdout);
    }
}

/*
 * scsd_dlm_client_send_op - node-A's generic DLM client SEND (vms-6ca, H5).
 * Builds and sends one DLM message on our DLM client CDT toward the peer's DLM
 * server handle (the same addressing scsd_dlm_send_enq resolves). Used for the
 * H5 sequence's second $ENQ and the holder's $DEQ. Returns 1 on send.
 */
static int scsd_dlm_client_send_op(struct scsd_rx *rx, struct peer_state *ps,
                                   uint8_t op, uint8_t mode, uint32_t req_lkid,
                                   uint32_t master_lkid, const char *resname,
                                   const char *label)
{
    if (rx == NULL || ps == NULL || ps->pb == NULL || ps->joiner_remote_conid == 0)
        return 0;

    uint32_t dlm_server = (ps->joiner_remote_conid & 0xFFFFFFF0u) |
                          OVMX_CONID_CLS_DLMSRV;

    struct scs_dlm_params p;
    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, ps_port_addr(ps), 6);
    memcpy(p.src_mac, rx->our_hw_mac, 6);
    memcpy(p.src_logical, rx->our_src_logical, 6);
    memcpy(p.peer_logical, ps_sys_addr(ps), 6);
    p.local_conid = PS_DLM_CONID(ps);
    p.remote_conid = dlm_server;
    p.recv_ack = ps->vc.seq.recv_seq;
    p.send_seq = scs_seq_advance(&ps->vc.seq);
    p.incarnation = ps->incarnation;

    struct scs_dlm_msg m;
    memset(&m, 0, sizeof(m));
    m.op = op;
    m.mode = mode;
    m.req_lkid = req_lkid;
    m.master_lkid = master_lkid;
    m.req_csid = resolve_scssystemid();
    if (resname != NULL) {
        size_t rl = strlen(resname);
        if (rl > SCS_DLM_RESNAM_MAX)
            rl = SCS_DLM_RESNAM_MAX;
        m.namelen = (uint8_t)rl;
        memcpy(m.resnam, resname, rl);
    }

    uint8_t frame[SCS_DLM_FRAME_LEN];
    if (scs_dlm_build_frame(&p, &m, frame) == 0 &&
        send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb, label, frame,
                      sizeof(frame)) > 0)
        return 1;
    return 0;
}

/*
 * scsd_dlm_client_send_deq_valblk - node A's cross-node $DEQ that WRITES the LVB
 * (rd vms-d81, H8). Same DLM-server addressing as scsd_dlm_client_send_op, but it
 * carries the 16-byte value block AND sets LCK_M_VALBLK so the master replicates
 * the WIRE value into res->valblk (vms_lock_dlm_xnode_deq). The flag bit is the
 * IOCTL LCK_M_VALBLK (0x08), not the starlet LCK$M_VALBLK (0x0001): the wire
 * flags field is copied VERBATIM into vms_dlm_xnode_args.flags by
 * scsd_dlm_dispatch_to_executive, and the executive's cross-node $DEQ handler
 * tests `req->flags & LCK_M_VALBLK`. Returns 1 on send.
 */
static int scsd_dlm_client_send_deq_valblk(struct scsd_rx *rx, struct peer_state *ps,
                                           uint32_t master_lkid, const char *resname,
                                           const uint8_t valblk[LCK$C_VALBLK_LEN],
                                           const char *label)
{
    if (rx == NULL || ps == NULL || ps->pb == NULL || ps->joiner_remote_conid == 0)
        return 0;

    uint32_t dlm_server = (ps->joiner_remote_conid & 0xFFFFFFF0u) |
                          OVMX_CONID_CLS_DLMSRV;

    struct scs_dlm_params p;
    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, ps_port_addr(ps), 6);
    memcpy(p.src_mac, rx->our_hw_mac, 6);
    memcpy(p.src_logical, rx->our_src_logical, 6);
    memcpy(p.peer_logical, ps_sys_addr(ps), 6);
    p.local_conid = PS_DLM_CONID(ps);
    p.remote_conid = dlm_server;
    p.recv_ack = ps->vc.seq.recv_seq;
    p.send_seq = scs_seq_advance(&ps->vc.seq);
    p.incarnation = ps->incarnation;

    struct scs_dlm_msg m;
    memset(&m, 0, sizeof(m));
    m.op = SCS_DLM_OP_DEQ;
    m.mode = LCK$K_NLMODE;
    m.flags = LCK_M_VALBLK;              /* IOCTL flag bit -- see comment above */
    m.master_lkid = master_lkid;
    m.req_csid = resolve_scssystemid();  /* the releasing node's identity */
    if (resname != NULL) {
        size_t rl = strlen(resname);
        if (rl > SCS_DLM_RESNAM_MAX)
            rl = SCS_DLM_RESNAM_MAX;
        m.namelen = (uint8_t)rl;
        memcpy(m.resnam, resname, rl);
    }
    memcpy(m.valblk, valblk, LCK$C_VALBLK_LEN);

    uint8_t frame[SCS_DLM_FRAME_LEN];
    if (scs_dlm_build_frame(&p, &m, frame) == 0 &&
        send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb, label, frame,
                      sizeof(frame)) > 0)
        return 1;
    return 0;
}

/*
 * scsd_dlm_cli_msg_input - node-A's DLM CLIENT input routine (vms-164d rung-1b;
 * vms-6ca rung H5). The GRANT / queued-reply the peer sent back lands here (dest
 * Con.ID = our PS_DLM_CONID). Decode it and complete the pending request.
 *
 * Rung 1b: record the status and print DLMDONE (the round-trip proof).
 *
 * Rung H5 (armed by OVMX_DLM_H5): drive the block-then-grant-over-the-wire
 * sequence, keyed on the request handle (req_lkid) and the reply status the
 * MASTER genuinely sent -- never a local decision (INV-6):
 *   - GRANT for #1 (status SS$_NORMAL): our holder is granted; remember B's
 *     master handle for it, then send the SECOND, incompatible $ENQ (#2).
 *   - queued-reply for #2 (status VMS_DLM_STS_QUEUED, mode NL): dispatch it into
 *     OUR executive (VMS_DLM_OP_GRANT receive) so #2's ORIGIN record is genuinely
 *     PENDING, read it back (GETLKI -> NL), then $DEQ the holder (#1).
 *   - deferred GRANT for #2 (status SS$_NORMAL, mode EX): dispatch it into our
 *     executive; the origin record FLIPS NL->EX -- the status flip observed on
 *     the REQUESTER node, read back (GETLKI -> EX) as the H5 proof.
 */
static void scsd_dlm_cli_msg_input(struct scs_cdt *cdt, const void *msg,
                                   size_t msglen, void *ctx)
{
    (void)cdt;
    (void)msg;
    (void)msglen;
    struct peer_state *ps = (struct peer_state *)ctx;
    struct scsd_rx_frame cur = scsd_rx_current;
    struct scs_dlm_view v;

    if (ps == NULL || cur.frame == NULL ||
        scs_dlm_parse(cur.frame, (size_t)cur.len, &v) != 0 ||
        (v.msg.op != SCS_DLM_OP_GRANT && v.msg.op != SCS_DLM_OP_BLKAST)) {
        return;
    }

    /* THE BLKAST WIRE, HOLDER SIDE (vms-76d, H6). A BLKAST node B sent because a
     * conflicting request queued behind our held #1 lands here. Dispatch it into
     * OUR executive, which FIRES the blocking AST on our holder process for real
     * (drained + proven by scsd_dlm_blkast_fire), THEN release the holder (#1) --
     * the holder releases in genuine RESPONSE to the BLKAST, not on its own. */
    if (v.msg.op == SCS_DLM_OP_BLKAST) {
        struct scsd_rx *rxb = scsd_rx_current.rx;
        if (rxb == NULL || !scsd_member_initiate_enabled() || !ps->dlm_h6)
            return;
        log_ts(stdout);
        printf(" SCSD-I-DLMBLKAST, RECEIVED BLKAST from master CSID=%u for holder"
               " req_lkid=0x%08X resnam='%.*s' -- a cross-node request blocks our"
               " held lock; firing our blocking AST\n",
               (unsigned)v.msg.master_csid, (unsigned)v.msg.req_lkid,
               (int)v.msg.namelen, v.msg.resnam);
        fflush(stdout);

        uint32_t delivered = 0;
        uint64_t drained = 0;
        uint32_t st = scsd_dlm_blkast_fire(v.msg.req_lkid, (const char *)v.msg.resnam,
                                           &delivered, &drained);
        if (st == 1u && delivered && drained != 0) {
            ps->dlm_blkast_fired = 1;
            log_ts(stdout);
            printf(" SCSD-I-DLMBLKFIRE, blocking AST DELIVERED on this node: BLKAST"
                   " receive rc=0x%08X delivered=%u drained astadr=0x%016llX -- a"
                   " REAL user-mode AST fired on the holder's executive, drained via"
                   " DELIVERAST (H6 proof, not the holder-releases-on-its-own"
                   " shortcut)\n",
                   (unsigned)st, (unsigned)delivered,
                   (unsigned long long)drained);
            fflush(stdout);
        } else {
            log_ts(stdout);
            printf(" SCSD-W-DLMBLKAST, BLKAST receive did NOT fire an AST"
                   " (rc=0x%08X delivered=%u drained=0x%016llX) -- honest, no fake\n",
                   (unsigned)st, (unsigned)delivered, (unsigned long long)drained);
            fflush(stdout);
        }

        /* Now release the holder (#1) in RESPONSE to the BLKAST -- the real $DEQ
         * that unblocks the queued #2 and drives the H5 deferred-grant flip. */
        if (!ps->dlm_deq1_sent && ps->dlm_master_lkid1 != 0) {
            if (scsd_dlm_client_send_op(rxb, ps, SCS_DLM_OP_DEQ, LCK$K_NLMODE,
                                        0, ps->dlm_master_lkid1,
                                        (const char *)v.msg.resnam,
                                        "OVMX$DLM DEQ #1 (H6 release on BLKAST)")) {
                ps->dlm_deq1_sent = 1;
                log_ts(stdout);
                printf(" SCSD-I-DLMDEQ1, released holder #1 (master_lkid=0x%08X) in"
                       " RESPONSE to the BLKAST -- expect the master to GRANT #2 and"
                       " WIRE the deferred GRANT (H6)\n",
                       (unsigned)ps->dlm_master_lkid1);
                fflush(stdout);
            }
        }
        return;
    }

    ps->dlm_grant_recv = 1;
    ps->dlm_grant_status = v.msg.status;
    log_ts(stdout);
    /* rd vms-e84: master_lkid added to this existing print (not a new send
     * site -- v.msg.master_lkid was already decoded off the wire by
     * scs_dlm_parse() above, just not previously logged here) so the e84
     * directory-refusal harness can report the REAL granted lock id on the
     * positive control without a separate GETLKI round-trip. 0 on a refusal
     * (dlm_resolve_master's early return never allocates a lock -- honest). */
    printf(" SCSD-I-DLMDONE, cross-node $ENQ round-trip COMPLETE: peer"
           " (master CSID=%u) answered GRANT status=0x%08X resnam='%.*s'"
           " req_lkid=0x%08X master_lkid=0x%08X mode=%s -- LIVE A->B->A"
           " transport proven; %s\n",
           (unsigned)v.msg.master_csid, (unsigned)v.msg.status,
           (int)v.msg.namelen, v.msg.resnam, (unsigned)v.msg.req_lkid,
           (unsigned)v.msg.master_lkid, scs_dlm_mode_name(v.msg.mode),
           v.msg.status == 1u
               ? "cross-node lock GRANTED by the master (SS$_NORMAL)"
               : "lock NOT granted (honest)");
    fflush(stdout);

    /* DISTRIBUTED DEADLOCK SEARCH, CONTENDER SIDE (rd vms-ec75, rung H11).
     *   - Our WAIT $ENQ (#2) QUEUED (status VMS_DLM_STS_QUEUED, mode NL): dispatch
     *     the queued-reply into OUR executive so grant_recv creates the PENDING
     *     origin record -- the wait-for edge VMS_IOCTL_DLM_ENUM_WAITS later surfaces
     *     when this node is chased. INV-6: a REAL origin, from the master's genuine
     *     queued reply, never a fabricated pending state.
     *   - Our WAIT $ENQ (#2) was ABORTED (status SS$_DEADLOCK): the master detected
     *     a cross-node deadlock cycle and chose us as the global-min victim; our
     *     $ENQ completes with SS$_DEADLOCK, exactly as the local detector's would. */
    if (ps->dlm_ec75 && v.msg.req_lkid == 2u) {
        if (v.msg.status == 0u) {   /* VMS_DLM_STS_QUEUED: pending, not granted */
            (void)scsd_dlm_dispatch_to_executive(&v.msg, NULL, NULL, NULL);
            log_ts(stdout);
            printf(" SCSD-I-DLKPEND, WAIT $ENQ '%.*s' QUEUED on the master; PENDING"
                   " origin recorded on this node (the wait-for edge the search"
                   " chases) -- read from OUR executive (H11)\n",
                   (int)v.msg.namelen, v.msg.resnam);
            fflush(stdout);
        } else if (v.msg.status == SS_DEADLOCK_VMS) {
            ps->dlm_ec75_victim_seen = 1;
            log_ts(stdout);
            printf(" SCSD-I-DLKVICTIM, our WAIT $ENQ '%.*s' (req_lkid=0x%08X)"
                   " returned status=0x%08X SS$_DEADLOCK -- this node is the"
                   " deterministic global-min victim; the distributed deadlock is"
                   " broken by aborting exactly this one request (H11, rd vms-ec75)\n",
                   (int)v.msg.namelen, v.msg.resnam, (unsigned)v.msg.req_lkid,
                   (unsigned)v.msg.status);
            fflush(stdout);
        }
        return;
    }

    /* THE LVB WIRE, WRITER SIDE (rd vms-d81, H8). On the GRANT for our holder
     * (#1), WRITE a known 16-byte value block and release it with a cross-node
     * $DEQ carrying LCK_M_VALBLK -- the master (node B) replicates the wire value
     * into res->valblk, and a LOCAL $ENQ on B then reads back exactly this value.
     * Independent of the H5/H6 sequence (no contention, no BLKAST): a single ENQ
     * then a value-block $DEQ. Emit the REAL bytes we wrote, hex-encoded (INV-6). */
    if (ps->dlm_h8 && v.msg.op == SCS_DLM_OP_GRANT &&
        v.msg.req_lkid == 1u && v.msg.status == 1u && !ps->dlm_deq1_sent) {
        struct scsd_rx *rxh8 = scsd_rx_current.rx;
        if (rxh8 != NULL && scsd_member_initiate_enabled()) {
            ps->dlm_master_lkid1 = v.msg.master_lkid;
            if (scsd_dlm_client_send_deq_valblk(rxh8, ps, ps->dlm_master_lkid1,
                                                (const char *)v.msg.resnam,
                                                scsd_h8_lvb,
                                                "OVMX$DLM DEQ (H8 LVB write)")) {
                ps->dlm_deq1_sent = 1;
                char hx[33];
                scsd_hex16(scsd_h8_lvb, hx);
                log_ts(stdout);
                printf(" SCSD-I-DLMLVBWR, name=%.*s val=%s -- wrote a 16-byte LVB"
                       " and released the holder (master_lkid=0x%08X) with a"
                       " cross-node $DEQ carrying LCK_M_VALBLK; the master"
                       " replicates this into res->valblk (H8)\n",
                       (int)v.msg.namelen, v.msg.resnam, hx,
                       (unsigned)ps->dlm_master_lkid1);
                fflush(stdout);
            }
        }
        return;
    }

    /* THE LVB READ CROSSING, REQUESTER SIDE (rd vms-eeb, rung H9). Our cross-node
     * $ENQ (#1, EX, VALBLK) was GRANTED and the GRANT frame carried the master's
     * value block. DISPATCH the grant into OUR executive so grant_recv stores that
     * LVB on our origin record, then GETLKI our OWN request handle and read it back
     * -- the master's res->valblk, transported B->A over SCS. The bytes are the
     * REAL executive state (zeros if none was delivered -> the runner FAILs). */
    if (ps->dlm_h9 && !ps->dlm_h9_read_done &&
        v.msg.req_lkid == 1u && v.msg.status == 1u) {
        ps->dlm_h9_read_done = 1;
        /* Store the GRANT's LVB into our executive-resident origin record. */
        (void)scsd_dlm_dispatch_to_executive(&v.msg, NULL, NULL, NULL);
        uint8_t lvb[LCK$C_VALBLK_LEN];
        uint32_t st = scsd_dlm_h9_read_valblk(v.msg.req_lkid, lvb);
        char hex[33];
        scsd_hex16(lvb, hex);
        log_ts(stdout);
        printf(" SCSD-I-DLMLVBRD9 name=%.*s val=%s (GETLKI req_lkid=0x%08X"
               " rc=0x%08X) -- read the master's LVB back from OUR executive after"
               " a cross-node $ENQ; the value block crossed the wire B->A on the"
               " GRANT (H9, the read crossing, mirror of the H8 write crossing)\n",
               (int)v.msg.namelen, v.msg.resnam, hex,
               (unsigned)v.msg.req_lkid, (unsigned)st);
        fflush(stdout);
        return;
    }

    /* THE HOLD, REQUESTER SIDE (rd vms-dca9, rung H10b). Our cross-node $ENQ
     * (#1, EX) to RES_C's master (node C) was GRANTED. Dispatch the GRANT into
     * OUR executive so our origin record genuinely reflects the held lock --
     * this is the PRE-DEPARTURE HOLD that must SURVIVE C's remaster. The
     * departure hook (scsd_dlm_h10b_rebuild_on_depart) re-reads it fresh via
     * GETLKI and re-registers it on the new master. */
    if (ps->dlm_h10b && !ps->dlm_h10b_hold_established &&
        v.msg.req_lkid == 1u && v.msg.status == 1u) {
        ps->dlm_h10b_hold_established = 1;
        (void)scsd_dlm_dispatch_to_executive(&v.msg, NULL, NULL, NULL);
        size_t rl = v.msg.namelen;
        if (rl > sizeof(ps->dlm_h10b_resnam) - 1)
            rl = sizeof(ps->dlm_h10b_resnam) - 1;
        memcpy(ps->dlm_h10b_resnam, v.msg.resnam, rl);
        ps->dlm_h10b_resnam[rl] = '\0';
        log_ts(stdout);
        printf(" SCSD-I-DLMHOLDOK name=%.*s master_csid=%u req_lkid=1 mode=%s --"
               " our cross-node lock is GRANTED and held; the PRE-DEPARTURE hold"
               " that must survive the master's remaster (H10b, rd vms-dca9)\n",
               (int)v.msg.namelen, v.msg.resnam, (unsigned)v.msg.master_csid,
               scs_dlm_mode_name(v.msg.mode));
        fflush(stdout);
        return;
    }

    if (!ps->dlm_h5)
        return;   /* rung 1b only: no H5 sequence armed */

    struct scsd_rx *rx = scsd_rx_current.rx;
    if (rx == NULL || !scsd_member_initiate_enabled())
        return;

    /* GRANT for our holder (#1): remember B's master handle, then contend (#2). */
    if (v.msg.req_lkid == 1u && v.msg.status == 1u) {
        ps->dlm_master_lkid1 = v.msg.master_lkid;
        /* H6 (vms-76d): establish our holder (#1) ORIGIN record WITH a blocking-AST
         * routine, so the BLKAST the master will send when #2 contends can fire a
         * REAL AST on us. (H5 never dispatched #1's grant -- the holder released on
         * its own; H6 makes the holder a genuine BLKAST target.) */
        if (ps->dlm_h6 && !ps->dlm_holder_armed) {
            uint64_t blkastadr = (uint64_t)(uintptr_t)&scsd_dlm_blkast_handler;
            uint32_t est = scsd_dlm_holder_establish(1u, LCK$K_EXMODE,
                                                     ps->dlm_master_lkid1,
                                                     (const char *)v.msg.resnam,
                                                     blkastadr);
            if (est == 1u) {
                ps->dlm_holder_armed = 1;
                log_ts(stdout);
                printf(" SCSD-I-DLMHOLDARM, holder #1 origin established WITH a"
                       " blocking-AST routine (blkastadr=0x%016llX rc=0x%08X) -- ready"
                       " to receive a BLKAST over SCS (H6)\n",
                       (unsigned long long)blkastadr, (unsigned)est);
                fflush(stdout);
            } else {
                log_ts(stdout);
                printf(" SCSD-W-DLMHOLDARM, holder #1 origin establish rc=0x%08X"
                       " (no blkast target) -- honest\n", (unsigned)est);
                fflush(stdout);
            }
        }
        if (!ps->dlm_enq2_sent) {
            if (scsd_dlm_client_send_op(rx, ps, SCS_DLM_OP_ENQ, LCK$K_EXMODE,
                                        2u /*req_lkid*/, 0, (const char *)v.msg.resnam,
                                        "OVMX$DLM ENQ #2 (H5 contend)")) {
                ps->dlm_enq2_sent = 1;
                log_ts(stdout);
                printf(" SCSD-I-DLMENQ2, sent SECOND cross-node $ENQ (req_lkid=2,"
                       " EX) -- incompatible with our held #1, expect it to QUEUE"
                       " on the master (H5)\n");
                fflush(stdout);
            }
        }
        return;
    }

    /* Replies for our contending request (#2). */
    if (v.msg.req_lkid == 2u) {
        /* Dispatch the reply into OUR executive so #2's origin record reflects
         * the master's genuine status; read the record back (GETLKI). */
        uint32_t origin_mode = 0xFFu;
        uint32_t st = scsd_dlm_dispatch_to_executive(&v.msg, NULL, NULL,
                                                     &origin_mode);

        if (v.msg.status == 0u) {   /* VMS_DLM_STS_QUEUED: pending, not granted */
            /* QUEUED-reply: the request is genuinely PENDING on the requester. */
            ps->dlm_pend_seen = 1;
            log_ts(stdout);
            printf(" SCSD-I-DLMPEND, cross-node $ENQ #2 QUEUED on the master;"
                   " requester origin record PENDING (recv rc=0x%08X granted_mode=%s)"
                   " -- genuine block, read from OUR executive\n",
                   (unsigned)st, scs_dlm_mode_name((uint8_t)origin_mode));
            fflush(stdout);
            /* Release the holder (#1): the real $DEQ that unblocks #2. In H6 this
             * is DEFERRED until the BLKAST arrives (the holder releases in RESPONSE
             * to the blocking AST, driven from the BLKAST branch above); H5 releases
             * here (no BLKAST wire, the holder releases on its own). */
            if (!ps->dlm_h6 && !ps->dlm_deq1_sent && ps->dlm_master_lkid1 != 0) {
                if (scsd_dlm_client_send_op(rx, ps, SCS_DLM_OP_DEQ, LCK$K_NLMODE,
                                            0, ps->dlm_master_lkid1, (const char *)v.msg.resnam,
                                            "OVMX$DLM DEQ #1 (H5 release holder)")) {
                    ps->dlm_deq1_sent = 1;
                    log_ts(stdout);
                    printf(" SCSD-I-DLMDEQ1, released holder #1 (master_lkid=0x%08X)"
                           " -- expect the master to GRANT #2 and WIRE the deferred"
                           " GRANT back (H5)\n", (unsigned)ps->dlm_master_lkid1);
                    fflush(stdout);
                }
            }
        } else if (v.msg.status == 1u) {
            /* DEFERRED GRANT: the origin record flips NL->EX -- THE H5 PROOF. */
            ps->dlm_flip_seen = 1;
            log_ts(stdout);
            printf(" SCSD-I-DLMH5FLIP, requester origin FLIPPED NL->%s for #2"
                   " (recv rc=0x%08X GETLKI granted_mode=%s) -- a QUEUED cross-node"
                   " request, GRANTED by a real remote $DEQ, delivered over the LIVE"
                   " SCS wire; block-then-grant proven on the REQUESTER node\n",
                   scs_dlm_mode_name((uint8_t)origin_mode), (unsigned)st,
                   scs_dlm_mode_name((uint8_t)origin_mode));
            fflush(stdout);
        }
        return;
    }
}

/*
 * scsd_dlm_send_enq - node-A's SEND path (vms-164d rung-1b). Once the join to
 * this peer is complete (its VMS$VAXcluster VC bound at JOINBOUND, so we know
 * its Con.ID base), issue ONE OVMX$DLM ENQ over the LIVE VC. Triggered by the
 * lab switch OVMX_DLM_ENQ=<resource-name>; one-shot per peer.
 *
 * DLM SYSAP addressing WITHOUT a separate connect round-trip (OVMX design
 * choice, labelled): the peer allocates its DLM SERVER handle deterministically
 * as PS_DLM_SERVER_CONID = base | slot | 0x000F, sharing the SAME base+slot as
 * the VMS$VAXcluster server handle it already taught us at JOINBOUND
 * (joiner_remote_conid, class 0x0001). We therefore RESOLVE its DLM server
 * Con.ID by substituting the class nibble -- no fabricated handle, the exact
 * value the peer's scsd_dlm_ensure_server_cdt() registered. The peer's server
 * CDT is unbound (remote_conid==0), so scs_cdl_resolve() admits our frame and
 * learns our client handle from the envelope; symmetrically our client CDT
 * (also unbound) admits its GRANT. Gated on the member-role flag.
 */
static void scsd_dlm_send_enq(struct scsd_rx *rx, struct peer_state *ps)
{
    if (rx == NULL || ps == NULL || ps->pb == NULL) {
        return;
    }
    const char *resname = getenv("OVMX_DLM_ENQ");
    if (resname == NULL || resname[0] == '\0' || !scsd_member_initiate_enabled()) {
        return;
    }
    /* rd vms-dca9 (H10b) / CORRECTED vms-e84: in a >2-node cluster the ENQ must
     * reach the resource's REAL directory node, not whichever peer's join tick
     * happens to fire first. The reason is NOT that the receiver would falsely
     * master a resource it doesn't own -- vms_lock_dlm_xnode_dispatch() grants
     * through vms_enq_core_ex() -> dlm_resolve_master(), which ALREADY refuses a
     * non-directory node (dir_csid != vms_local_csid -> SS$_UNSUPPORTED, well
     * before it masters). That refusal is RUNTIME-PROVEN on a 3-node rail by
     * tests/qemu/run_dlm_harness_e84.sh (RES_C sent to non-directory B answers
     * SS$_UNSUPPORTED=0x8F8; sent to directory C answers SS$_NORMAL). The
     * earlier H10b note here CLAIMED the receiver masters unconditionally with
     * "no receive-side mastership check" -- that was FALSE (a code-read error
     * caught + refuted under vms-e84); this comment corrects the record.
     * So the targeting is for DETERMINISM: address the ENQ to the directory so
     * it is GRANTED rather than honestly refused, which is what a harness that
     * must ESTABLISH a real cross-node hold needs. Opt-in and additive: absent
     * (every H5-H9 harness is two-node, so every peer was always the directory
     * for the name), every peer is still a legal target, byte-identical to
     * before this rung existed. Directory REDIRECT -- forwarding a mis-addressed
     * ENQ to the real directory instead of refusing it, which would let a
     * harness drop this targeting crutch -- is the deferred follow-on to e84. */
    const char *target_csid_env = getenv("OVMX_DLM_ENQ_CSID");
    if (target_csid_env != NULL && target_csid_env[0] != '\0') {
        uint32_t want_csid = (uint32_t)strtoul(target_csid_env, NULL, 10);
        if ((uint32_t)peer_node_number(ps) != want_csid) {
            return;
        }
    }
    /* Only after the join is complete: JOINBOUND taught us joiner_remote_conid,
     * which anchors the peer's Con.ID base+slot. */
    if (!ps->joiner_connected || ps->joiner_remote_conid == 0 || ps->dlm_enq_sent) {
        return;
    }

    struct scs_cdt *cli = scs_cdl_lookup(&scsd_cdl, PS_DLM_CONID(ps));
    if (cli == NULL) {
        cli = scs_cdl_alloc_conid(&scsd_cdl, PS_DLM_CONID(ps),
                                  "OVMX$DLM", "OVMX$DLM", ps->pb);
    }
    if (cli != NULL) {
        scs_cdt_set_handlers(cli, scsd_dlm_cli_msg_input, NULL, NULL, ps);
    }

    /* vms-6ca (H5): arm the block-then-grant-over-the-wire sequence if requested.
     * The cli input routine drives the rest (second $ENQ, $DEQ, deferred-grant
     * flip) off the GRANT replies. A no-op without OVMX_DLM_H5 (rung 1b only). */
    if (getenv("OVMX_DLM_H5") != NULL)
        ps->dlm_h5 = 1;

    /* vms-76d (H6): arm the BLKAST wire, layered on H5. H6 implies the H5 sequence
     * (holder + contend + block-then-grant); it ADDS establishing the holder WITH a
     * blocking-AST routine and driving the holder's release off a RECEIVED BLKAST. */
    if (getenv("OVMX_DLM_H6") != NULL) {
        ps->dlm_h5 = 1;
        ps->dlm_h6 = 1;
    }

    /* rd vms-d81 (H8): arm the LVB wire. A single ENQ then a value-block $DEQ --
     * NOT the H5/H6 contention/BLKAST sequence, so H8 does NOT set dlm_h5/dlm_h6.
     * The cli input routine (dlm_h8 branch) drives the WRITE off the GRANT. */
    if (getenv("OVMX_DLM_H8") != NULL)
        ps->dlm_h8 = 1;

    /* vms-eeb (H9): arm the LVB READ crossing -- INDEPENDENT of H5/H6 (no
     * contention, no BLKAST). Node A does ONE cross-node $ENQ (EX) that carries
     * LCK_M_VALBLK so the master READS its res->valblk back into the GRANT reply;
     * the cli input routine then GETLKIs our own handle and reads the LVB. */
    if (getenv("OVMX_DLM_H9") != NULL)
        ps->dlm_h9 = 1;

    /* rd vms-dca9 (H10b): arm the PRE-DEPARTURE HOLD + REMASTER REBUILD.
     * Node A's cross-node $ENQ to RES_C's master must SURVIVE that master's
     * departure -- INDEPENDENT of H5/H6/H8/H9 (no contention, no VALBLK). The
     * GRANT branch in scsd_dlm_cli_msg_input establishes the real origin
     * record; the departure hook (scsd_dlm_h10b_rebuild_on_depart) drives the
     * rebuild send. */
    if (getenv("OVMX_DLM_H10B") != NULL)
        ps->dlm_h10b = 1;

    uint32_t dlm_server = (ps->joiner_remote_conid & 0xFFFFFFF0u) |
                          OVMX_CONID_CLS_DLMSRV;

    struct scs_dlm_params p;
    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, ps_port_addr(ps), 6);
    memcpy(p.src_mac, rx->our_hw_mac, 6);
    memcpy(p.src_logical, rx->our_src_logical, 6);
    memcpy(p.peer_logical, ps_sys_addr(ps), 6);
    p.local_conid = PS_DLM_CONID(ps);
    p.remote_conid = dlm_server;
    p.recv_ack = ps->vc.seq.recv_seq;
    p.send_seq = scs_seq_advance(&ps->vc.seq);
    p.incarnation = ps->incarnation;

    struct scs_dlm_msg m;
    memset(&m, 0, sizeof(m));
    m.op = SCS_DLM_OP_ENQ;
    m.mode = LCK$K_EXMODE;
    /* vms-eeb (H9): carry LCK_M_VALBLK (the IOCTL flag 0x08, NOT starlet 0x0001)
     * so the master READS its res->valblk back into the GRANT reply. A zero input
     * block means "read the master's LVB", not "write mine". */
    if (ps->dlm_h9)
        m.flags |= LCK_M_VALBLK;
    m.req_lkid = 1;                        /* our local lock handle for this request */
    m.req_csid = resolve_scssystemid();    /* our node identity */
    size_t rl = strlen(resname);
    if (rl > SCS_DLM_RESNAM_MAX) {
        rl = SCS_DLM_RESNAM_MAX;
    }
    m.namelen = (uint8_t)rl;
    memcpy(m.resnam, resname, rl);

    uint8_t frame[SCS_DLM_FRAME_LEN];
    if (scs_dlm_build_frame(&p, &m, frame) == 0 &&
        send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                      "OVMX$DLM ENQ (rung-1b request)", frame,
                      sizeof(frame)) > 0) {
        ps->dlm_enq_sent = 1;
        log_ts(stdout);
        printf(" SCSD-I-DLMENQ, sent cross-node $ENQ resnam='%.*s' mode=%s"
               " to peer DLM server 0x%08X (our client 0x%08X) over LIVE VC\n",
               (int)m.namelen, m.resnam, scs_dlm_mode_name(m.mode),
               (unsigned)dlm_server, (unsigned)PS_DLM_CONID(ps));
        fflush(stdout);
    }
}

/*
 * scsd_dlm_ensure_server_cdt - allocate (once) this node's DLM server CDT for a
 * peer and point it at scsd_dlm_srv_msg_input, so a DLM message arriving on
 * PS_DLM_SERVER_CONID(ps) is delivered to the handler above. Mirrors the MSCP
 * server CDT registration. "OVMX$DLM" is a LABELLED OVMX SYSAP name (no public
 * VMS DLM SYSAP name is documented; the real lock manager rides the connection
 * manager's VC -- OVMX gives the DLM its own SYSAP per the rung-1 scope, an
 * OVMX design choice recorded in docs/compat/facilities/cluster-dlm.yaml).
 */
static struct scs_cdt *scsd_dlm_ensure_server_cdt(struct peer_state *ps)
{
    if (ps == NULL)
        return NULL;
    struct scs_cdt *dcdt = scs_cdl_lookup(&scsd_cdl, PS_DLM_SERVER_CONID(ps));
    if (dcdt == NULL) {
        dcdt = scs_cdl_alloc_conid(&scsd_cdl, PS_DLM_SERVER_CONID(ps),
                                   "OVMX$DLM", "OVMX$DLM", ps->pb);
    }
    if (dcdt != NULL) {
        scs_cdt_set_handlers(dcdt, scsd_dlm_srv_msg_input, NULL, NULL, ps);
    }
    return dcdt;
}

/*
 * mscp_connect_retx_timeout_ms - vms-694: the grounded ~10s MSCP$DISK
 * CONNECT-REQUEST retransmit cadence (MSCP_CONNECT_RETX_TIMEOUT_MS above),
 * with a lab-only override. Same shape as scsd_shutdown_wait_ms() below:
 * OVMX_MSCP_CONNECT_RETX_MS, a non-negative decimal milliseconds value, lets
 * a lab run trade authenticity for turnaround time deliberately and
 * visibly -- it must never be the default path, so an absent/malformed/
 * negative value always falls back to the grounded constant rather than to
 * some other guess.
 */
static long mscp_connect_retx_timeout_ms(void)
{
    const char *e = getenv("OVMX_MSCP_CONNECT_RETX_MS");
    if (e != NULL && e[0] != '\0') {
        char *end = NULL;
        long v = strtol(e, &end, 10);
        if (end != NULL && *end == '\0' && v >= 0) {
            return v;
        }
    }
    return (long)MSCP_CONNECT_RETX_TIMEOUT_MS;
}

/*
 * scsd_join_retx_for_peer - vms-694: the join-sequencer's per-step CONNECT-
 * REQUEST retransmit (both the JS_IDLE closure-branch joiner retransmit and
 * the worktree-760 step sequencer), factored out of scsd_handle_frame()'s
 * directed-HELLO branch so it is reachable from TWO places: unchanged, from
 * that same branch (so a peer that keeps directing HELLO at OVMX sees no
 * behaviour change), and NEW, from scsd_join_retx_tick() on the main loop's
 * own clock.
 *
 * WHY THE SECOND CALLER. Before this, the block below ran ONLY when a
 * directed HELLO arrived FROM THIS PEER -- the original comment's own words
 * were "Coarsely HELLO-driven". A live lab-2 rejoin (vms-694, 2026-08-09)
 * showed exactly the failure mode that coupling invites: OVMX's join stuck at
 * JS_MSCP_CONNECT, js_retx frozen at 6 with JOIN_RETX_MAX=12 (six retries of
 * headroom unused), while the run's own trace showed HELLO and credit traffic
 * still flowing with BOTH peers for the rest of the run. Six retransmits and
 * then silence, capped well short of the retry ceiling, is what this function
 * being reachable only from a directed-HELLO branch produces the moment that
 * SPECIFIC peer's directed-HELLO flag to OVMX goes quiet (p. 2-14: an
 * established/verified channel has no architectural reason to keep directing
 * HELLO at OVMX) -- a condition that reflects the PEER's own channel-verify
 * state, not whether OVMX's MSCP$DISK connect ever got answered. OVMX's own
 * retransmit going silent with the peer never having refused it a further
 * time is fully explained without invoking any peer-side rate limiting: OVMX
 * simply stopped asking.
 *
 * scsd_join_retx_tick() closes that gap by driving this SAME logic from the
 * main loop's SO_RCVTIMEO-driven wakeup, independent of what frame (if any)
 * last arrived from this peer. Both callers gate on the same
 * ps->js_last_tx / ps->last_joiner_req timestamps, so running from both sites
 * cannot double-send within one JOIN_RETX_TIMEOUT_MS window -- whichever
 * caller runs first in a given window wins, and the other sees the guard
 * clock ineligible.
 */
static void scsd_join_retx_for_peer(struct scsd_rx *rx, struct peer_state *ps, long now_ms)
{
    if (rx == NULL || ps == NULL) {
        return;
    }

    /* vms-694 (spec 4(O.35)) NEGATIVE CONTROL (default OFF; NOT a fix). Pre-emptively
     * open OVMX's VMS$VAXcluster joiner VC to every member already at VC OPEN
     * (start_acked -> an open circuit), off the main-loop clock, instead of waiting
     * for the member-driven SCS$DIRECTORY prompt. This TESTS whether the win-vs-lose
     * one-VC-vs-two-VC difference is OVMX-forceable -- and REFUTES it: the member
     * transport-accepts (CONNECT ACK) but never sends ACCEPT_REQUEST for a connect
     * it did not itself solicit, so the VC stalls and the return LOSES 0/10 (see
     * ovmx_join_all_members()). Kept as the bisectable control that grounds the
     * 4(O.35) verdict. A no-op under pure-server / no-own-VC / the join sequencer. */
    if (ovmx_join_all_members() && rx->do_connect && !rx->join_seq_enabled &&
        ps->start_acked && !ps->joiner_connected && !ps->joiner_connect_sent &&
        getenv("OVMX_PURE_SERVER") == NULL && getenv("OVMX_NO_OWN_VC") == NULL) {
        if (send_joiner_connect_request(rx->sock, rx->ifindex, rx->cfg, ps, NULL,
                                        rx->our_hw_mac, rx->our_src_logical)) {
            rx->connect_req_sent++;
            log_ts(stdout);
            printf(" SCSD-I-CONNREQ, sent OUR VMS$VAXcluster CONNECT-REQUEST"
                   " local_conid=0x%08X seq=%u (spec 4(O.35) all-members"
                   " total-connectivity)\n",
                   PS_JOINER_CONID(ps), ps->joiner_req_seq);
            fflush(stdout);
        }
    }

    if (ps->join_step == JS_IDLE && scsd_joiner_retransmit_pending(rx, ps)) {
        long last_ms = ps->last_joiner_req.tv_sec * 1000L +
                       ps->last_joiner_req.tv_nsec / 1000000L;
        if ((now_ms - last_ms) >= 1000) {
            /* vms-398: name the NODE, not the circuit -- CONNECT picks
             * the OPEN virtual circuit via CONFIG_SYS (p. 2-47). */
            if (send_joiner_connect_request(rx->sock, rx->ifindex, rx->cfg, ps, NULL,
                                            rx->our_hw_mac, rx->our_src_logical)) {
                rx->connect_req_sent++;
                log_ts(stdout);
                printf(" SCSD-I-CONNREQ, retransmit OUR VMS$VAXcluster"
                       " CONNECT-REQUEST local_conid=0x%08X seq=%u\n",
                       PS_JOINER_CONID(ps), ps->joiner_req_seq);
                fflush(stdout);
            }
        }
    }

    /* vms-760: the join SEQUENCER drives each step's PROMPT send from the
     * receive path (SCSD-I-JOINSEQ). Here we only RETRANSMIT the current
     * outstanding step (reusing its stored send_seq -- never advancing, or
     * we reopen the 760mscp hole) if its response has not arrived, capped
     * at JOIN_RETX_MAX. */
    if (rx->do_connect && ps->start_acked && ps->join_step != JS_IDLE &&
        ps->join_step != JS_DONE && ps->join_step != JS_ADD_MEMBER &&
        ps->js_retx < (ps->appeared_after_join ? JOIN_GREET_RETX_MAX
                                               : JOIN_RETX_MAX)) {
        long last_ms = ps->js_last_tx.tv_sec * 1000L +
                       ps->js_last_tx.tv_nsec / 1000000L;
        /* vms-694: JS_MSCP_CONNECT alone rides the grounded ~10s MSCP$DISK
         * CONNECT-REQUEST cadence -- every other step here keeps the generic
         * JOIN_RETX_TIMEOUT_MS, which this measurement does not cover. */
        long step_timeout_ms = (ps->join_step == JS_MSCP_CONNECT)
                                    ? mscp_connect_retx_timeout_ms()
                                    : (long)JOIN_RETX_TIMEOUT_MS;
        if ((now_ms - last_ms) >= step_timeout_ms) {
            int sent = 0;
            switch (ps->join_step) {
            case JS_DIR_CONNECT:
                sent = send_own_dir_connect_request(rx->sock, (int)rx->ifindex, ps,
                                                    rx->our_hw_mac, rx->our_src_logical);
                clock_gettime(CLOCK_MONOTONIC, &ps->js_last_tx);
                break;
            case JS_DIR_LOOKUP_TAPE:
            case JS_DIR_LOOKUP_DISK:
            case JS_DIR_LOOKUP_VC:
                sent = send_own_dir_lookup(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                           rx->our_src_logical, NULL, 1);
                break;
            case JS_MSCP_CONNECT:
                sent = send_mscp_connect_request(rx->sock, (int)rx->ifindex, ps,
                                                 rx->our_hw_mac, rx->our_src_logical);
                clock_gettime(CLOCK_MONOTONIC, &ps->js_last_tx);
                break;
            case JS_VC_CONNECT:
                /* vms-578: the vms-398 signature -- name the NODE, not the
                 * circuit; CONNECT picks the OPEN VC via CONFIG_SYS (p. 2-47). */
                sent = send_joiner_connect_request(rx->sock, rx->ifindex, rx->cfg, ps, NULL,
                                                   rx->our_hw_mac, rx->our_src_logical);
                clock_gettime(CLOCK_MONOTONIC, &ps->js_last_tx);
                break;
            default:
                break;
            }
            if (sent) {
                ps->js_retx++;
                log_ts(stdout);
                printf(" SCSD-I-JOINSEQ, retransmit join step %d (retx %u)\n",
                       ps->join_step, ps->js_retx);
                fflush(stdout);
            }
        }
    }
}

static void scsd_handle_frame(struct scsd_rx *rx, const uint8_t *buf, ssize_t n)
{
    if (n < 14) {
        return; /* shorter than a bare Ethernet header -- ignore */
    }

    /* The socket protocol filter already restricts delivery to
     * ethertype 0x6007, but re-check explicitly: it documents intent
     * and matches the absolute frame-offset convention (spec sec 2 /
     * dissect_sca.py) where the SCA payload begins at offset 14. */
    uint16_t ethertype = (uint16_t)(((unsigned)buf[12] << 8) | buf[13]);
    if (ethertype != SCA_ETHERTYPE) {
        return;
    }

    const uint8_t *sca_payload = buf + 14;
    size_t payload_len = (size_t)n - 14;

    uint16_t total_sca_len = 0;
    scs_class_t cls = scs_classify_sca_payload(sca_payload, payload_len, &total_sca_len);

    rx->counts[cls]++;
    rx->total_frames++;

    /* vms-17f: the peer is alive. Stamped for EVERY 0x6007 frame carrying a
     * known peer's source MAC, before the unicast-to-us gate below, because a
     * node that is only beaconing multicast has not departed. Allocates
     * nothing -- an unknown source is ignored here and picked up (or not) by
     * the branch that handles its frame. */
    peer_touch(rx->peers, buf + OFF_ETH_SRC, monotonic_ms());

    log_ts(stdout);
    printf(" SCSD-I-FRAME, class=%-9s total_sca_len=%u eth_len=%zd"
           " src=%02x:%02x:%02x:%02x:%02x:%02x dst=%02x:%02x:%02x:%02x:%02x:%02x\n",
           scs_class_name(cls), total_sca_len, n,
           buf[6], buf[7], buf[8], buf[9], buf[10], buf[11],
           buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
    fflush(stdout);

    /* --- vms-f3e: OVMX<->OVMX bring-up -- the MEMBER-SIDE SOLICIT.
     *
     * The responder below acts ONLY on frames unicast to our HW MAC, because
     * the whole design assumes "our own multicast beacon prompts the PEER's
     * directed HELLO": against a real VAX, a running MEMBER hears a new node's
     * multicast HELLO and INITIATES a directed HELLO (abs-30 = PFW_INIT/b2,
     * "member's first directed contact") to solicit it. OVMX has no member-side
     * solicit -- it only ever REPLIES to a directed HELLO -- so two OVMX joiners
     * both multicast and both wait forever for a directed HELLO that neither
     * sends. The NISCA channel never forms, join_step never leaves JS_IDLE, and
     * nothing climbs past HELLO (rd vms-f3e two-OVMX baseline, this file's
     * tests/cluster/two-ovmx harness).
     *
     * KILL-SWITCH DISCIPLINE (CLAUDE.md Rule 8, INV-6): this fires ONLY when
     * OVMX_MCAST_SOLICIT is set. Absent the flag the multicast-return path below
     * is UNCHANGED, so OVMX's bytes toward a real VAX stay byte-identical -- a
     * VAX is the member and does the soliciting; OVMX must not also solicit it.
     * The value on the wire is GROUNDED, not invented: PFW_INIT (0xb2) is the
     * documented member-first-contact abs-30 word (scs_hello.h). One directed
     * HELLO per peer per second, and only until the channel is up, so it merely
     * bootstraps the handshake the existing (a)/padded paths then carry. */
    if (rx->respond && (buf[OFF_ETH_DST] & 0x01) &&        /* multicast dst   */
        cls == SCS_CLASS_HELLO &&
        !mac_eq(buf + OFF_ETH_SRC, rx->our_hw_mac) &&       /* not our beacon  */
        getenv("OVMX_MCAST_SOLICIT") != NULL) {
        const uint8_t *sm = buf + OFF_ETH_SRC;
        struct peer_state *sps =
            peer_find_or_add(rx->cfg, rx->pdt, rx->peers, sm);
        if (sps != NULL && !sps->channel_up) {
            ps_learn_sys_addr(rx->cfg, sps, buf + OFF_HELLO_SRCLOG);
            struct timespec snow;
            clock_gettime(CLOCK_MONOTONIC, &snow);
            int sdue = (sps->directed_replies == 0 && sps->last_directed.tv_sec == 0) ||
                       (snow.tv_sec - sps->last_directed.tv_sec >= 1);
            if (sdue) {
                uint8_t sframe[SCS_HELLO_FRAME_LEN];
                rx->hello_params->timer_tick = hello_timer_tick100();
                memcpy(rx->hello_params->peer_logical, buf + OFF_HELLO_SRCLOG, 6);
                if (scs_hello_build_directed_frame(rx->hello_params, sm, rx->lab_nonce,
                                                   SCS_HELLO_JOINER_INCARNATION,
                                                   SCS_HELLO_PFW_INIT, sframe) == 0 &&
                    send_frame_channel(rx->sock, rx->ifindex, sm, sframe,
                                       sizeof(sframe)) > 0) {
                    rx->directed_sent++;
                    sps->last_directed = snow;
                    log_ts(stdout);
                    printf(" SCSD-I-MCASTSOLICIT, OVMX_MCAST_SOLICIT: sent"
                           " member-first directed HELLO (abs30 b2/PFW_INIT) to"
                           " %02x:%02x:%02x:%02x:%02x:%02x heard on multicast\n",
                           sm[0], sm[1], sm[2], sm[3], sm[4], sm[5]);
                    fflush(stdout);
                }
            }
        }
    }

    /* --- vms-5fe responder --- only act on frames unicast to our HW MAC
     * (our own multicast beacon prompts the peer's directed HELLO). */
    if (!rx->respond || !mac_eq(buf + OFF_ETH_DST, rx->our_hw_mac)) {
        return;
    }
    const uint8_t *src_mac = buf + OFF_ETH_SRC;

    /* vms-fdd: DECODE AND LOG THE PEER'S CONNECT DATA (p. 2-25). Sited here,
     * once, ahead of every branch, deliberately: the connect frames of the
     * different SYSAPs are answered in several different places below (the
     * joiner-accept bind (b1), the SCS$DIRECTORY responder (b2), and the
     * member-opened VMS$VAXcluster accept (c)), several of which `return`, so
     * a per-branch log would silently miss whichever SYSAP's frame took
     * another path. scs_connect_data_get() is the filter -- it accepts ONLY
     * the 110-byte class with connection-control message type CONNECT_REQ(0)
     * or ACCEPT_REQ(2) -- so this fires on exactly the connect frames and on
     * nothing else, and it reads no state and emits no frame. */
    (void)scsd_log_peer_connect_data(buf, (size_t)n);

    /* --- vms-4071: THE IMPLIED ACK (p. 2-16). "what if, in the meantime,
     * the remote node performs an operation that requires the circuit to be
     * OPEN, and this operation results in a packet being sent to the local
     * node? In this case, SCA states that the local node will treat that
     * packet as an 'implied ACK' and simply mark the circuit as being
     * OPEN." Only the SCS sequenced-message classes qualify -- 0x41 and the
     * HELLO are datagrams (p. 2-40) and precede the circuit, so feeding
     * them here would open circuits that are still forming. Dormant on a
     * clean join: the member does not send circuit traffic before the
     * dialogue finishes. */
    if (rx->do_connect && n >= 32 && scs_vc_is_circuit_packet(buf[30])) {
        struct scs_pb *ipb = scs_config_find_pb(rx->cfg, rx->pdt, src_mac);
        if (ipb != NULL && ipb->vc_state == SCS_VC_START_RECEIVED) {
            struct peer_state *ips = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
            enum scs_vc_action iact =
                scs_vc_fsm_recv(ipb, SCS_VC_EV_OTHER, monotonic_ms());
            log_ts(stdout);
            printf(" SCSD-I-VCIMPACK, implied ACK from peer"
                   " %02x:%02x:%02x:%02x:%02x:%02x (opcode 0x%02x) -- VC %s\n",
                   src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5],
                   buf[30], scs_vc_state_name(ipb->vc_state));
            fflush(stdout);
            if (ips != NULL && ips->pb == ipb) {
                /* peer_round2_seen=0: no round-2 frame arrived, and none
                 * ever will -- scsd_vc_ack_due() lets fsm.implied_acks
                 * stand in for it so the dialogue does not deadlock. */
                scsd_vc_settle(rx->vc_ctx, ips, iact, 0, &rx->start_ack_sent);
            }
        }
    }
    /* --- vms-760 server-first: answer VAX1's op8->op9 / op6->op7 SCS
     * connection credit/ready handshake on ANY of OUR connections. VAX1 runs
     * this after a connection binds and GATES admission (incl. accepting the
     * joiner's own client connects) on it. rc@64 = OUR conid (peer addresses
     * us); op@60 in {6,8}. Reflect it (scs_reflect_credit). --- */
    /* vms-760: answering the peer's op6/op8 connection-management handshake is a
     * PROTOCOL requirement on every path, not a pure-server nicety -- the real
     * joiner (VAX3) answers them while driving its own connects. Previously gated
     * to pure-server, so the sequencer path left them unanswered. */
    /* vms-2f3 sec 4M.14: ...AND ITS RETRANSMIT FORM, 0x7b. THIRD instance of
     * the same defect -- sec 4d.10 fixed it on the connection-manager path and
     * the 0x5b comment below fixed it once before that, but THIS site, the one
     * whose own comment says VAX1 "GATES admission" on the handshake, still
     * gated on 0x4b||0x5b and silently discarded every retransmission.
     *
     * Measured 7/7 on lab-2 with bracketing controls: VAX1 sends op=8 to us in
     * EVERY run, and only in the four REFUSED rejoins does it retransmit --
     * twice, 3.0 s apart (the collapsed ReXmt interval of sec 4d.9), as 72-byte
     * mt=0x7b addressed to SCS_DIR_OVMX_CONID. The three joins are answered
     * first time and never retransmit, so a fresh identity never meets 0x7b
     * here and the deafness is unreachable on the path we test. That is exactly
     * the sec 4L.9h shape.
     *
     * OVMX_NO_CREDIT_RETX=1 restores the deaf behaviour (guardrail 21). */
    if (rx->do_connect && n >= 72 &&
        (buf[30] == SCS_MSGTYPE_SEQAPP || buf[30] == SCS_DIR_OPCODE ||
         (buf[30] == SCS_DIR_OPCODE_RETX &&
          getenv("OVMX_NO_CREDIT_RETX") == NULL))) {
        uint16_t cm_op = (uint16_t)buf[60] | ((uint16_t)buf[61] << 8);
        uint32_t cm_rc = (uint32_t)buf[64] | ((uint32_t)buf[65] << 8) |
                         ((uint32_t)buf[66] << 16) | ((uint32_t)buf[67] << 24);
        /* vms-578 INTEGRATION -- THE TWO BRANCHES READ [46:48]==6 AS THE SAME
         * THING UNDER DIFFERENT NAMES, AND THE ARCHITECTED READING WINS.
         *
         * worktree-760 called {6, 8} a "credit/ready handshake" and answered 6
         * by calling scs_send_disconnect() -- its own comment says "complete the
         * bidirectional teardown ... fully closing the server dir connection".
         * That IS the DISCONNECT_REQ -> DISCONNECT_RSP pairing that spec sec
         * 4(h)(1a) grounds as message types 6 and 7, and that vms-591 already
         * implements properly: Figure 2-16's two arrows, the p. 2-26 symmetric
         * own-disconnect, the p. 2-27 simultaneous case and the reason code, all
         * driven off the CDT by the vms-dd5 classifier in (b1) below.
         *
         * This branch RETURNS, so leaving 6 in it swallowed every DISCONNECT
         * before the classifier ran. MEASURED: 17 assertions across
         * test_reason_real_disconnect_req_is_decoded_and_logged,
         * test_peer_disconnect_req_is_answered_and_matched,
         * test_simultaneous_disconnect_sends_no_second_request and
         * test_clean_shutdown_kill_switch_through_the_daemon went red on the
         * merged tree and green again with 6 removed from here.
         *
         * 8 STAYS. scs_conn_event_for_msgtype() names 0..7 and nothing else, so
         * message type 8 has no architected handler and worktree-760's
         * observation of it (op=8 to us in EVERY lab-2 run, 7/7) is the only
         * thing that answers it at all. It is left exactly as worktree-760 had
         * it, retransmit handling included. */
        if (cm_op == 8 &&
            (cm_rc & 0xFFFF0000u) == ovmx_conid_base()) {
            struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
            if (ps != NULL) {
                if (!ps->vc.initialized) {
                    scs_vc_init(&ps->vc);
                }
                scs_vc_note_recv(&ps->vc,
                    (uint16_t)buf[34] | ((uint16_t)buf[35] << 8));
                scs_reflect_credit(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                   rx->our_src_logical, buf, (size_t)n);
                /* vms-2f3 sec 4M.14: make the retransmit case visible. Every
                 * refused rejoin measured so far carries exactly two of these
                 * and every join carries none. */
                if (buf[30] == SCS_DIR_OPCODE_RETX) {
                    rx->credit_retx_seen++;
                    log_ts(stdout);
                    printf(" SCSD-I-CREDITRETX, answered a RETRANSMITTED (0x7b)"
                           " op%u credit/ready handshake on conid=0x%08X --"
                           " every previous build silently discarded this\n",
                           (unsigned)cm_op, cm_rc);
                    fflush(stdout);

                    /* vms-2f3 sec 4M.23/4M.24: THE PEER IS TELLING US IT IS
                     * STUCK. An op8 retransmission on OUR server directory
                     * conid happens in EVERY refused rejoin (2 per run,
                     * 3.0 s apart) and in NO join -- 7/7 measured. Its CDT is
                     * in disc_sent/disc_pend, i.e. it has performed its
                     * disconnect call and is waiting for ours (DTJ v1n5 p.25:
                     * the teardown is SYMMETRIC and "the SYSAP in the other
                     * node must then perform its own disconnect call").
                     * OVMX otherwise only ever emits op6 in REPLY to the
                     * peer's, which on a rejoin never comes -- so perform ours
                     * here, on the one signal that reliably marks the stall.
                     * Opt-in: OVMX_DIR_SELF_DISCONNECT=1. */
                    if (cm_op == 8 && ovmx_conid_is_class(cm_rc, OVMX_CONID_CLS_DIR) &&
                        !ps->psc_self_disc_sent &&
                        ps->dir_remote_conid != 0 &&
                        getenv("OVMX_DIR_SELF_DISCONNECT") != NULL &&
                        scs_send_disconnect_self(rx->sock, (int)rx->ifindex, ps,
                                                 rx->our_hw_mac, rx->our_src_logical)) {
                        ps->psc_self_disc_sent = 1;
                        log_ts(stdout);
                        printf(" SCSD-I-SELFDISC, peer retransmitted op8 and its"
                               " CDT is disconnect-pending -- performed OUR OWN"
                               " disconnect call (op 6) remote=0x%08X local=0x%08X."
                               " DTJ v1n5 p.25: the teardown is symmetric\n",
                               (unsigned)ps->dir_remote_conid,
                               (unsigned)PS_SCS_DIR_CONID(ps));
                        fflush(stdout);
                    }
                }
                /* DELETED (vms-096): a `if (cm_op == 6 && cm_rc ==
                 * SCS_DIR_OVMX_CONID)` block used to sit HERE -- INSIDE the
                 * `cm_op == 8` branch, where cm_op is 8 by construction. It was
                 * unreachable, and it carried real behaviour with it: the only
                 * call of scs_send_disconnect(), the only write to
                 * ps->psc_credit_done, and one of the two disk-discovery
                 * triggers. See the DISK DISCOVERY note above
                 * scsd_peer_departure_sweep()'s ungate block in main() for what
                 * remains and what was ruled. */
            }
            return;
        }
    }
    /* --- vms-691: VC engine -- credit-ack every sequenced message the peer
     * sends us. Each 0x5b directory / 0x4b connect / 190-byte VC message
     * (send_seq != 0 at [20:22], abs 34) is answered by EXACTLY ONE 0x48
     * credit-return (strict 1-for-1, spec sec 4h(3)). This is what stops
     * the VAX's "%PEA0 Excessive packet losses / Closed Virtual Circuit"
     * teardown. The 0x41 START phase uses its own config-round ack
     * mechanism (branch (b) below), so 0x41 is excluded here. We do NOT
     * `continue`: 0x4b/190 frames still fall through to branch (c) for
     * Con.ID binding. */
    if (rx->do_connect && n >= 36 && buf[31] == SCS_FORMAT_CONST &&
        (buf[30] == SCS_MSGTYPE_DIRLOOKUP || buf[30] == SCS_DIR_OPCODE_RETX ||
         buf[30] == SCS_MSGTYPE_SEQAPP || cls == SCS_CLASS_SCS_FIXED)) {
        uint16_t peer_send_seq = (uint16_t)(buf[34] | ((uint16_t)buf[35] << 8)); /* [20:22] */
        uint16_t peer_recv_ack = (uint16_t)(buf[32] | ((uint16_t)buf[33] << 8)); /* [18:20] */
        struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
        if (ps != NULL) {
            if (!ps->vc.initialized) {
                scs_vc_init(&ps->vc);
            }
            /* The peer's leading counter acks OVMX's own sequenced sends. */
            scs_vc_note_peer_ack(&ps->vc, peer_recv_ack);
            ps_learn_sys_addr(rx->cfg, ps, buf + OFF_HELLO_SRCLOG); /* src-logical, abs 24 */

            /* --- vms-abc: THE p. 2-31 SEQUENTIALITY GUARANTEE. "if either the
             * guarantee of message delivery or the guarantee of message
             * sequentiality cannot be satisfied, the virtual circuit between
             * the ports involved will be explicitly broken (if it isn't
             * already). If this happens, then every connection supported by
             * this virtual circuit is also broken, and the SYSAPs participating
             * in these connections are notified of the event."
             *
             * scs_vc_note_recv_checked() is scs_vc_note_recv() plus that check:
             * the recv_seq advance is identical, so nothing changes on the
             * in-order, duplicate/retransmit or first-message paths -- which is
             * every frame in every capture we hold (see the measurement in
             * scs_vc.h). Only a genuine gap behaves differently, and then OVMX
             * must NOT credit-ack: returning a 0x48 for a message that implies
             * others were lost tells the peer OVMX received messages it never
             * saw. That is what the code below used to do. */
            unsigned missing = 0;
            enum scs_vc_seq_verdict sv =
                scs_vc_note_recv_checked(&ps->vc, peer_send_seq, &missing);
            if (sv == SCS_VC_SEQ_GAP) {
                vc_seq_gaps++;
                log_ts(stdout);
                printf(" SCSD-W-SEQGAP, peer %02x:%02x:%02x:%02x:%02x:%02x sent"
                       " send_seq=%u with recv_seq=%u -- %u sequenced message(s)"
                       " missing; the p. 2-31 sequentiality guarantee has failed\n",
                       src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4],
                       src_mac[5], peer_send_seq,
                       (unsigned)(uint16_t)(peer_send_seq - missing - 1u), missing);
                fflush(stdout);
                unsigned broken = scs_vc_break(&ps->vc, ps->pb,
                                               SCS_VC_BREAK_SEQ_GAP, stdout);
                if (scs_vc_break_enabled()) {
                    vc_breaks++;
                    vc_conns_broken += broken;
                    /* vms-c7d: a sequentiality failure is a connectivity loss,
                     * NOT a last-gasp departure -- the peer did not announce it
                     * is leaving. Enter the CSB WAIT state and HOLD membership;
                     * scsd_recnx_tick() runs the once-a-second RECNXINTERVAL
                     * reconnect loop (transcript p. 7-30). */
                    scsd_recnx_note_vc_loss(ps, monotonic_ms());
                    /* The circuit is gone. No credit-return, and no further
                     * dispatch of a frame that arrived on it. */
                    return;
                }
                /* OVMX_NO_VC_BREAK: fall through and limp on, exactly as OVMX
                 * did before this item. */
            }

            if (scs_vc_owes_credit(peer_send_seq)) {
                uint8_t cframe[SCS_CREDIT_FRAME_LEN];
                if (scs_vc_build_credit_for(&ps->vc, ps_port_addr(ps), rx->our_hw_mac,
                                            rx->our_src_logical, ps_sys_addr(ps), cframe) == 0 &&
                    send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                                  "0x48 credit-return", cframe,
                                  sizeof(cframe)) > 0) {
                    ps->credit_sent++;
                    rx->credit_sent++;
                    log_ts(stdout);
                    printf(" SCSD-I-CREDIT, 0x48 credit-return acked peer_seq=%u"
                           " to %02x:%02x:%02x:%02x:%02x:%02x (#%ld)\n",
                           peer_send_seq,
                           ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                           ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
                           ps->credit_sent);
                    fflush(stdout);
                }
            }
        }
    }

    /* ===================== vms-7c0: THE p. 2-29 RECEIVE PATH ==================
     *
     * "the remote port driver uses the low order 16 bits of the destination
     *  CONID as an index into the remote node's CDL ... There it finds the
     *  address of the CDT ... The port driver then offsets to a fixed location
     *  within the CDT to locate the address of the remote SYSAP's message input
     *  routine, and passes the message to that routine."      (p. 2-29)
     *
     * WHAT THIS REPLACED. The vms-224 connection-manager block used to sit
     * here and decide, for itself, whether a 190-byte frame was addressed to
     * OVMX -- by comparing the frame's Con.ID pair against two compile-time
     * macros (`mv.remote_conid == OVMX_LOCAL_CONID || mv.local_conid ==
     * OVMX_LOCAL_CONID || mv.remote_conid == OVMX_JOINER_CONID || ...`). That
     * is the per-opcode ad-hoc dispatch the CDL exists to abolish: it can only
     * ever recognise the three Con.IDs somebody remembered to spell out, it
     * cannot tell a live connection from a released one, and it accepted a
     * frame whose SOURCE Con.ID belonged to a previous incarnation of the peer.
     *
     * Now the CDL answers all three questions in one lookup, and the block that
     * used to ask them is scsd_sysap_msg_input() -- the VMS$VAXcluster SYSAP's
     * p. 2-29 message input routine, installed on every CDT the five services
     * open. Nothing about the CM dialogue itself changed; what changed is WHO
     * decided the frame was ours.
     *
     * PEER-SUPPLIED INDEX -- the bound check is scs_cdl_lookup()'s and stays
     * there. h.dest_conid comes off the wire, and its low 16 bits select a CDL
     * slot; SCS_CDL_ENTRIES is 240 while the field spans 0..65535, so the
     * `slot >= SCS_CDL_ENTRIES` refusal in scs_cdl_lookup() is REACHABLE from
     * this call and is what stops a peer indexing past the table. On top of it
     * the lookup requires the slot to hold an IN-USE CDT whose full 32-bit
     * local Con.ID equals the value supplied, and scs_cdl_resolve() then
     * refuses a source Con.ID that is not this connection's remote handle
     * (p. 2-35). Nothing in this file indexes the CDL itself.
     *
     * THIS SENDS NOTHING. Every frame the delivered SYSAP goes on to emit still
     * leaves through send_frame_vc(), exactly as it did when the block was
     * inline -- the SEND SITE TABLE is unchanged by this item.
     * ======================================================================= */
    if (rx->do_connect && scsd_cdl_ready) {
        struct scs_rx_hdr h;
        if (scs_rx_parse(sca_payload, payload_len, &h) == 0) {
            /* vms-aa1: THE RECEIVE HALF OF THE FLOW-CONTROL ACCOUNT, p. 2-43/
             * 2-44. Done BEFORE delivery, deliberately: OVMX's SYSAP input
             * routines answer SYNCHRONOUSLY (scsd_sysap_msg_input() emits the
             * 0x81 CM response inside the delivery call), so a credit banked
             * after delivery would arrive one frame too late to pay for the
             * very reply the message provoked -- and the p. 2-43 order is
             * "adds the contents of the credit field to its Send Credit count"
             * on receipt, ahead of any send. Resolution is scs_cdl_resolve(),
             * the SAME p. 2-29/2-35 function scs_cdl_deliver_message() uses
             * below, so the account can never be credited for a frame the
             * delivery path then refuses. */
            struct scs_cdt *rcdt = NULL;   /* resolved connection, or NULL */
            struct scs_cdt *ccdt = NULL;   /* the one owing a p. 2-43 buffer release */
            /* vms-a61: NO scs_credit_header_offset() GATE HERE ANY MORE. `h`
             * already proves this frame envelope-conformant (scs_rx_parse
             * succeeded to produce it), and the credit field sits at the
             * SAME fixed content[48:50] on every conformant frame regardless
             * of length (scs_env.h) -- h.credit above is already that field,
             * read by the shared parse, not re-derived here. The length
             * allowlist scs_credit_header_offset() re-checks is therefore
             * redundant at this call site: it can only ever agree with the
             * envelope test on today's corpus (measured, zero missing
             * classes -- tools/cluster/scs_env_measure.py part G) and would
             * only ever DISAGREE by refusing a conformant length outside the
             * seven it lists, which is exactly the case this migration
             * removes the redundant refusal for. */
            if (scs_credit_enabled() &&
                scs_cdl_resolve(&scsd_cdl, h.dest_conid, h.src_conid, &rcdt) == SCS_DELIVER_OK) {
                if (h.kind == SCS_RX_APP_MESSAGE) {
                    /* p. 2-44: "remote SCS adds the contents of the credit
                     * field to its Send Credit count." The message also
                     * consumed one of this node's receive buffers, which is
                     * what scs_credit_on_recv() debits. */
                    (void)scs_credit_on_recv(rcdt, h.credit);
                    credit_recv_banked++;
                    credit_recv_units += h.credit;
                    ccdt = rcdt;
                } else if (h.mtype == SCS_ENV_MTYPE_ACCEPT_REQ && h.credit > 0) {
                    /* vms-ec7: spelled from the shared MTYPE namespace. Same
                     * value as the SCS_CONN_MSGTYPE_ACCEPT_REQ this used to
                     * read -- scs_connect.h's pair is scoped to the two classes
                     * that carry connect data -- but this is a DISPATCH site,
                     * and a dispatch site should name the namespace it is
                     * dispatching on. */
                    /* p. 2-43: the credit field of an ACCEPT_REQ is the number
                     * of Send Credits the remote SYSAP is EXTENDING at
                     * connection formation -- spec sec 4(g)'s tunable match,
                     * not a piggyback. This is where a connection's Send Credit
                     * comes from in the first place. No buffer was consumed:
                     * a connection-control message is not a SYSAP message. */
                    (void)scs_credit_grant_from_peer(rcdt, h.credit);
                    credit_grants_recv++;
                    credit_grant_units += h.credit;
                }
            }
            if (h.kind == SCS_RX_APP_MESSAGE) {
                rx_app_messages++;
                /* The daemon's receive context, for the duration of ONE
                 * delivery. OVMX DESIGN CHOICE, labeled: p. 2-29 hands the
                 * input routine "the message", and OVMX's SYSAP parsers
                 * (scs_member_parse) read ABSOLUTE frame offsets, so the
                 * routine is given the p. 4-15 payload as its argument AND can
                 * reach the frame it came out of through here. scsd is a
                 * single-threaded recv loop -- one frame is in flight at a
                 * time -- and the save/restore makes a nested delivery safe
                 * rather than merely unlikely. */
                struct scsd_rx_frame saved = scsd_rx_current;
                scsd_rx_current.rx = rx;
                scsd_rx_current.frame = buf;
                scsd_rx_current.len = n;
                int dres = scs_cdl_deliver_message(&scsd_cdl, h.dest_conid,
                                                   h.src_conid, h.payload,
                                                   h.payload_len);
                scsd_rx_current = saved;
                switch (dres) {
                case SCS_DELIVER_OK:
                    rx_delivered_message++;
                    /* p. 2-43: the SYSAP "has processed the contents of ...
                     * those buffers and released them back to local SCS". OVMX
                     * delivery is synchronous, so the routine returning IS the
                     * release, and the freed buffer becomes one Pending Receive
                     * Credit -- the quantity the NEXT outbound message on this
                     * connection piggybacks (p. 2-44). This is the only thing
                     * that ever makes a stamped credit non-zero. */
                    if (ccdt != NULL) {
                        (void)scs_credit_release_buffer(ccdt);
                        credit_buffers_released++;
                    }
                    break;
                case SCS_DELIVER_NO_CDT:
                    rx_deliver_no_cdt++;
                    break;
                case SCS_DELIVER_SRC_MISMATCH:
                    rx_deliver_src_mismatch++;
                    log_ts(stdout);
                    printf(" SCSD-W-RXSRCMISMATCH, application message for"
                           " conid=0x%08X carries source conid=0x%08X, which is"
                           " not that connection's remote handle (p. 2-35) --"
                           " not delivered\n",
                           (unsigned)h.dest_conid, (unsigned)h.src_conid);
                    fflush(stdout);
                    break;
                case SCS_DELIVER_NO_ROUTINE:
                default:
                    rx_deliver_no_routine++;
                    break;
                }
            } else if (h.kind == SCS_RX_CONTROL) {
                /* vms-ec7/vms-a61: THE CONTROL HALF OF THE p. 4-15 DISPATCH,
                 * decoded AND STEPPED at the one place every frame is
                 * classified. p. 4-15 routes on MTYPE: a message goes to the
                 * CDT's message input routine (the arm above), and a control
                 * message goes to the connection state machine instead.
                 *
                 * THIS USED TO RUN FROM A DIFFERENT PLACE. Through vms-a61,
                 * the actual conn_step() call sat at the (b1) classifier much
                 * further down this function, re-parsing the SAME frame with
                 * its own scs_env_parse_frame() call, behind its own
                 * `n>=72 && buf[30] in {0x4b,0x5b,0x7b}` marker gate -- so
                 * this, the one place that already holds the parsed envelope,
                 * could only count. vms-a61 moves the step itself here, onto
                 * `h`, and deletes the (b1) reparse.
                 *
                 * WHY THIS IS SAFE TO MOVE (not merely convenient), checked
                 * against every branch that runs between here and the old
                 * (b1) site before making the move:
                 *   - MTYPE 0 (CONNECT_REQ) and 2 (ACCEPT_REQ) are excluded
                 *     from stepping HERE exactly as they were at (b1) --
                 *     they are answered by the explicit CONNECT/ACCEPT
                 *     branches, which are untouched and still run at their
                 *     existing call sites (some of which are AFTER this
                 *     block; that is fine, since 0/2 are never stepped from
                 *     either location).
                 *   - No branch between here and the old (b1) site returns
                 *     for a frame carrying a conformant MTYPE in {1,3,4,5,6,
                 *     7}: the op6/op8 credit/ready block above only acts on
                 *     MTYPE 8 (an unnamed type, vms-f03, never stepped by
                 *     this machine either way); the VC-engine credit-ack
                 *     block's only `return` is on a sequence GAP, which is a
                 *     circuit break that would have stopped the frame from
                 *     ever reaching the old (b1) site too; the padded-HELLO,
                 *     directed-HELLO and START blocks below all gate on a
                 *     DIFFERENT wire marker (HELLO length / 0x41 opcode) that
                 *     an envelope-conformant control frame never carries, so
                 *     they never consume one of these frames either.
                 *   - When scsd_cdl_ready is 0 (OVMX_NO_CONN_FSM), this whole
                 *     block -- like the old (b1) lookup against an
                 *     uninitialized, zeroed scsd_cdl -- is a no-op either way.
                 *
                 * WHAT DOES CHANGE, NAMED RATHER THAN HIDDEN: the (b1) site's
                 * OWN extra gate (`n>=72 && buf[30] in {0x4b,0x5b,0x7b}`) is
                 * gone. Every conformant control frame in the corpus this
                 * item can measure against carries one of those three markers
                 * (docs/design-mscp-direction.md / scs_credit.h's "0x?B13
                 * family" note), so this is not observed to change anything
                 * TODAY -- but a hypothetical conformant control frame under
                 * a fourth, unobserved marker would now reach the state
                 * machine where it previously would not have. That is the
                 * same class of "wire-visible if it ever changes" the sibling
                 * changes in this item (scs_connect_parse's has_conid,
                 * scs_credit_header_offset) already accept, and it is why
                 * this item's own text calls for a lab-2 bracket: NOT run
                 * this turn (see the item's own note on that), so treat this
                 * paragraph as a recorded risk, not a closed one. */
                rx_control_messages++;
                if (h.mtype <= SCS_ENV_MTYPE_CONTROL_MAX) {
                    rx_control_by_mtype[h.mtype]++;
                }
                enum scs_conn_event cev;
                if (h.mtype != SCS_ENV_MTYPE_CONNECT_REQ &&
                    h.mtype != SCS_ENV_MTYPE_ACCEPT_REQ &&
                    scs_conn_event_for_msgtype(h.mtype, &cev)) {
                    struct scs_cdt *tgt = scs_cdl_lookup(&scsd_cdl, h.dest_conid);
                    if (tgt != NULL) {
                        /* vms-6b3: THE REASON CODE, decoded before the
                         * transition so the log reads in wire order -- see
                         * the grounding at scs_reason.h. Gated on the same
                         * Con.ID ownership test as the transition. */
                        if (scs_reason_carried_by(h.mtype)) {
                            uint16_t reason = 0;
                            if (scs_reason_get(buf, (size_t)n, h.mtype, &reason) == 1) {
                                conn_reason_seen++;
                                if (reason != 0) {
                                    conn_reason_nonzero++;
                                }
                                log_ts(stdout);
                                printf(" SCSD-I-CONNREASON, conid=0x%08X: %s"
                                       " carries reason code %u (%s)%s\n",
                                       (unsigned)h.dest_conid,
                                       h.mtype == SCS_REASON_MSGTYPE_REJECT_REQ
                                           ? "REJECT_REQ" : "DISCONNECT_REQ",
                                       (unsigned)reason, scs_reason_name(reason),
                                       reason == 0 ? " -- no reason supplied,"
                                                     " which is what every VMS"
                                                     " frame we have observed"
                                                     " carries" : "");
                                fflush(stdout);
                            }
                        }
                        /* vms-591: THE DISCONNECT MESSAGES ARE ANSWERED, NOT
                         * MERELY RECORDED -- see scsd_disconnect_dialogue().
                         * lconid (abs 68, the peer's own handle) is recorded
                         * before the dialogue runs: OVMX must address its
                         * DISCONNECT_RSP and its own DISCONNECT_REQ to the
                         * peer's Con.ID, and on a connection OVMX did not open
                         * the CDT may not have it yet. A frame carrying
                         * remote Con.ID 0 names no connection at the peer. */
                        if (cev == SCS_CONN_EV_RCV_DISCONNECT_REQ ||
                            cev == SCS_CONN_EV_RCV_DISCONNECT_RSP) {
                            struct peer_state *dps =
                                peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
                            if (dps != NULL) {
                                uint32_t lconid = h.src_conid;
                                if (lconid != 0) {
                                    scs_cdt_set_remote_conid(tgt, lconid);
                                }
                                int rel = scsd_disconnect_dialogue(rx->sock, rx->ifindex,
                                                                   dps, tgt, cev,
                                                                   rx->our_hw_mac,
                                                                   rx->our_src_logical);
                                /* vms-66f r4: TELL THE OWNING SYSAP -- see the
                                 * grounding at the (former) (b1) site. */
                                if (rel && scsd_poller_ready &&
                                    scs_poll_cdt_released(&scsd_poller, tgt)) {
                                    log_ts(stdout);
                                    printf(" SCSD-I-POLLCLOSED, the Process"
                                           " Poller's SCS$DIRECTORY connection"
                                           " reached CLOSED (p. 2-26) -- the"
                                           " p. 2-50 cycle ended clean,"
                                           " descriptor returned\n");
                                    fflush(stdout);
                                }
                            } else {
                                conn_step(tgt, cev, NULL);
                            }
                        } else {
                            struct scs_conn_transition ct = conn_step(tgt, cev, NULL);
                            /* vms-257: THE HONEST MSCPSRVOK. The member's op=3
                             * ACCEPT_RSP is the confirm of OUR server-side accept;
                             * this classifier steps it through the FSM. Report the
                             * server connection "confirmed" ONLY when that step
                             * actually reached OPEN on OUR MSCP$DISK server handle
                             * -- never optimistically, and never off a REJECT_RSP.
                             * This replaces the old op=5-as-confirm log that fired
                             * while the FSM was still CLOSED (INV-6). */
                            if (cev == SCS_CONN_EV_RCV_ACCEPT_RSP && !ct.illegal &&
                                ct.to == SCS_CONN_OPEN &&
                                ovmx_conid_is_class(h.dest_conid,
                                                    OVMX_CONID_CLS_MSCPSRV)) {
                                struct peer_state *ops =
                                    peer_find_or_add(rx->cfg, rx->pdt, rx->peers,
                                                     src_mac);
                                if (ops != NULL && !ops->mscp_srv_confirmed) {
                                    ops->mscp_srv_confirmed = 1;
                                    ops->mscp_srv_remote_conid = h.src_conid;
                                    log_ts(stdout);
                                    printf(" SCSD-I-MSCPSRVOK, member confirmed OUR"
                                           " MSCP$DISK server connection (FSM"
                                           " reached OPEN): local=0x%08X"
                                           " remote=0x%08X\n",
                                           (unsigned)h.dest_conid,
                                           (unsigned)h.src_conid);
                                    fflush(stdout);
                                }
                            }
                        }
                    }
                }
            } else if (h.kind == SCS_RX_UNKNOWN_MTYPE) {
                /* Never observed in 981,367 envelope-conformant frames across
                 * 141 reference-lab pcaps -- see the census in scs_rx.h. This
                 * counter is how "OVMX has never seen an SCS message type
                 * outside {0..10}" stays a MEASUREMENT instead of a memory, and
                 * it is the counter that would move the day an application
                 * DATAGRAM class finally shows up. THE COUNTS ABOVE ARE A DATED
                 * SNAPSHOT, THE CONCLUSION IS NOT. The reference lab keeps
                 * writing captures, so the corpus GROWS and a later re-derivation
                 * will not reproduce these totals -- a re-run on 2026-08-05 read
                 * 154 pcaps / 1,002,247 envelope-conformant frames (942,251
                 * real-VAX-source) and got MTYPE 0..10 with MTYPE 10 at 923,678 /
                 * 23.9% credit-0. A DIFFERENT TOTAL IS NOT A REGRESSION; a
                 * different SHAPE would be. What must still hold on re-run, and
                 * what the code depends on, is: the MTYPE namespace is exactly
                 * {0..10}, MTYPE 10 dominates, and no eleventh value appears. If
                 * an MTYPE outside {0..10} ever shows up, scsd's rx_unknown_mtype
                 * counter moves and the datagram question is finally answerable. */
                rx_unknown_mtype++;
                log_ts(stdout);
                printf(" SCSD-W-RXMTYPE, SCS message type %u is outside the"
                       " observed {0..10} namespace (dest=0x%08X src=0x%08X"
                       " total_sca=%u) -- not classified, not delivered\n",
                       (unsigned)h.mtype, (unsigned)h.dest_conid,
                       (unsigned)h.src_conid, (unsigned)h.total_sca_len);
                fflush(stdout);
            }
        }
    }

    /* (a0) vms-9f3: NISCA channel packet-size verification (spec sec 4k).
     * An ESTABLISHED VAX1 zero-pads a directed HELLO up to NISCS_MAX_PKTSZ
     * (1500 SCA / 1514 wire) and retransmits it (~6s: 1500->1069->853->745)
     * until OVMX RECIPROCATES with its OWN padded HELLO on the reverse
     * channel. That reciprocal is the "ack" VAX1 waits on before opening
     * OVMX's CSB and driving the sec 4j add-member commit -- an unpadded
     * 120-byte HELLO does NOT satisfy it (GROUNDED negative, sec 4k). We
     * reply to the SAME total size (capped at NISCS_MAX_PKTSZ), rate-limited
     * to ~5s so we never out-pace VAX1's ~6s probe cadence (reply-
     * amplification guard: do NOT flood 1500B frames). A padded HELLO does
     * not classify as SCS_CLASS_HELLO (length != 120), so it reaches here as
     * SCS_CLASS_OTHER and is detected by is_padded_hello(). */
    if (is_padded_hello(buf, (size_t)n)) {
        struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
        if (ps == NULL) {
            return;
        }
        ps_learn_sys_addr(rx->cfg, ps, buf + OFF_HELLO_SRCLOG); /* src-logical, abs 24 */
        uint16_t rx_sca = (uint16_t)((size_t)n - 14u); /* total SCA content received */
        if (rx_sca > ps->peer_padded_sca) {
            ps->peer_padded_sca = rx_sca;
        }
        /* The member advertises the node-incarnation it attributes to OVMX
         * in the padded HELLO's abs-92 flag (sec 4i.B); capture it so the
         * START [22:24] responder can echo it (that echo is the sec 4i.B
         * gate and is driven elsewhere -- left unchanged). */
        uint16_t adv_inc =
            (uint16_t)(buf[OFF_HELLO_DIRFLG] | ((uint16_t)buf[OFF_HELLO_DIRFLG + 1] << 8));
        if (adv_inc != 0) {
            ps->incarnation = adv_inc;
        }
        /* OVMX's OWN padded HELLO carries the incarnation OVMX attributes to
         * the PEER, which for a freshly-booted joiner's first contact is 1 --
         * NOT the member's advertised value. GROUNDED (spec sec 4i.B: "the
         * joiner's own directed HELLO always carries [78:80]=0x0001"), and
         * byte-exact in the golden formation-ci1 padded pair (BOTH the
         * joiner VAX2->VAX1 and member VAX1->VAX2 1500B frames carry abs-92
         * = 1). Stamping the echoed member incarnation here instead makes
         * VAX1 reject the channel-size verification and re-probe forever. */
        /* vms-d94: a padded HELLO is a channel-verify REQUEST (its abs-30
         * word is b3, spec sec 4a offset-30 / sec 4k). The GROUNDED ack to a
         * padded b3 probe is a PLAIN b4 CONFIRM HELLO -- byte-exact in
         * formation-clean-2node.pcap: VAXA->VAXB padded(1514) b3 @+34.2159 is
         * answered +0.2ms later by VAXB->VAXA PLAIN b4 @+34.2161 (and
         * symmetrically). OVMX previously RECIPROCATED a padded probe with its
         * OWN padded b3 (grounded on the degraded ci3 flood captures); on the
         * clean reference that never satisfies VAX1's size-verify -- VAX1's
         * probe needs a b4 ack it never got, so it re-floods forever and no
         * CSB opens. We now ACK each padded probe with a plain b4. OVMX's OWN
         * outbound size-verify (proving OVMX can SEND a full 1500B frame) is
         * the one-shot proactive padded b3 sent from the plain-HELLO path
         * below (guarded by padded_initiated). */
        uint8_t pad_resp_pfw = scs_hello_response_pfw(buf[30]); /* b3 -> b4 */
        ps_channel_up(ps);
        struct timespec pnow;
        clock_gettime(CLOCK_MONOTONIC, &pnow);
        uint8_t ackframe[SCS_HELLO_FRAME_LEN];
        rx->hello_params->timer_tick = hello_timer_tick100(); /* vms-9f3: live 100ns tick */
        /* vms-760: reply's SCA dest-logical (abs 16) = the PEER's logical
         * addr, read off abs 24 of the frame we are answering -- not its HW
         * MAC. See scs_hello.h. */
        memcpy(rx->hello_params->peer_logical, buf + 24, 6);
        if (scs_hello_build_directed_frame(rx->hello_params, src_mac, rx->lab_nonce,
                                           SCS_HELLO_JOINER_INCARNATION,
                                           pad_resp_pfw, ackframe) == 0 &&
            send_frame_channel(rx->sock, rx->ifindex, src_mac, ackframe, sizeof(ackframe)) > 0) {
            ps->padded_replies++;
            ps->last_padded = pnow;
            rx->directed_sent++;
            log_ts(stdout);
            printf(" SCSD-I-PADACK, acked padded HELLO probe (%u SCA rx, abs30 %02x->%02x)"
                   " with plain b4 CONFIRM to %02x:%02x:%02x:%02x:%02x:%02x (ack #%ld)\n",
                   rx_sca, buf[30], pad_resp_pfw,
                   src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5],
                   ps->padded_replies);
            fflush(stdout);
        }
        return;
    }

    /* (a) Directed HELLO -> reply with our directed HELLO (NISCA channel,
     * spec sec 4b). The frame is already known to be unicast to our HW MAC
     * (gated above), so any non-zero directed flag marks a directed HELLO
     * aimed at us. Observed on the wire: an established member uses 0x0001,
     * but a member SOLICITING a not-yet-joined node (us) uses 0x0002 --
     * accept both. */
    if (cls == SCS_CLASS_HELLO && n >= OFF_HELLO_DIRFLG + 2 &&
        (buf[OFF_HELLO_DIRFLG] != 0x00 || buf[OFF_HELLO_DIRFLG + 1] != 0x00)) {
        struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
        if (ps == NULL) {
            return;
        }
        ps_learn_sys_addr(rx->cfg, ps, buf + OFF_HELLO_SRCLOG);

        /* vms-af2/vms-691/vms-9f3: the member advertises the node-incarnation
         * it attributes to OVMX in its directed-HELLO flag [78:80] (abs 92),
         * LE u16. Capture it -- OVMX echoes it into its 0x41 START [22:24]
         * (the established-join gate, spec sec 4i.B; driven by the START
         * responder from ps->incarnation). READ off the wire; never a
         * hard-coded constant. 1 for a fresh/first contact.
         *
         * CRITICAL (spec sec 4i.B, GROUNDED on the golden wire): the value
         * OVMX stamps in its OWN directed HELLO abs 92 is NOT this advertised
         * value -- "the joiner's own directed HELLO always carries
         * [78:80]=0x0001". abs 92 is the incarnation the SENDER attributes to
         * the PEER, which for a freshly-booted joiner's first contact is 1.
         * The member's advertised value belongs ONLY in the START [22:24].
         * Echoing it back into OVMX's own HELLO abs 92 (the earlier bug)
         * leaves VAX1's PEDRIVER channel unverifiable -- it re-probes the
         * padded-HELLO ladder and re-sends START round-0 forever, and no CSB
         * ever opens. */
        uint16_t adv_incarnation =
            (uint16_t)(buf[OFF_HELLO_DIRFLG] | ((uint16_t)buf[OFF_HELLO_DIRFLG + 1] << 8));
        if (adv_incarnation != 0 && adv_incarnation != ps->incarnation) {
            ps->incarnation = adv_incarnation;
            log_ts(stdout);
            printf(" SCSD-I-INCARN, member advertises node-incarnation N=%u"
                   " (abs 92 [78:80]) -- will echo into START [22:24]"
                   " (own HELLO abs 92 stays %u)\n",
                   adv_incarnation, (unsigned)SCS_HELLO_JOINER_INCARNATION);
            fflush(stdout);
        }

        /* Rate-limit our directed replies to a real-node cadence (~1/s per
         * peer). Replying 1:1 to every received directed HELLO amplifies
         * into a tight ping-pong storm with the peer; real nodes beacon on
         * a timer instead. */
        struct timespec dnow;
        clock_gettime(CLOCK_MONOTONIC, &dnow);
        int due = (ps->directed_replies == 0) ||
                  (dnow.tv_sec - ps->last_directed.tv_sec >= 1);
        if (due) {
            uint8_t dframe[SCS_HELLO_FRAME_LEN];
            rx->hello_params->timer_tick = hello_timer_tick100(); /* vms-9f3: live 100ns tick */
            /* vms-d94: reply per the GROUNDED abs-30 channel-verify rule
             * (spec sec 4a offset-30): the received per-frame word buf[30]
             * (b2/b3/b4) drives our reply word -- b2->b3, b3->b4 (the fix
             * that makes the member see OVMX CONFIRM the channel), b4->b3
             * (re-initiate). OVMX previously held a fixed b3 and never sent
             * b4, so VAX1's NISCA handshake never finalized (vms-5fe). */
            uint8_t resp_pfw = scs_hello_response_pfw(buf[30]);
            /* abs 92 = SCS_HELLO_JOINER_INCARNATION (1): OVMX's own directed
             * HELLO carries the incarnation it attributes to the peer on a
             * fresh first contact, NOT the member's advertised value (spec
             * sec 4i.B). The member's value is echoed only into START [22:24]. */
            /* vms-760: SCA dest-logical (abs 16) = the PEER's logical addr
             * from abs 24 of the probe, not its HW MAC (see scs_hello.h). */
            memcpy(rx->hello_params->peer_logical, buf + 24, 6);
            if (scs_hello_build_directed_frame(rx->hello_params, src_mac, rx->lab_nonce,
                                               SCS_HELLO_JOINER_INCARNATION,
                                               resp_pfw, dframe) == 0 &&
                send_frame_channel(rx->sock, rx->ifindex, src_mac, dframe, sizeof(dframe)) > 0) {
                rx->directed_sent++;
                ps->directed_replies++;
                ps_channel_up(ps);
                ps->last_directed = dnow;
                log_ts(stdout);
                printf(" SCSD-I-DIRHELLO, replied directed HELLO (abs30 %02x->%02x)"
                       " to peer %02x:%02x:%02x:%02x:%02x:%02x (reply #%ld)\n",
                       buf[30], resp_pfw,
                       src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5],
                       ps->directed_replies);
                fflush(stdout);
            }
        } else {
            ps_channel_up(ps);
        }

        /* vms-9f3: match the golden joiner (VAX2) -- once our directed
         * channel is up, proactively send ONE padded HELLO up to
         * NISCS_MAX_PKTSZ to advertise OVMX's own channel size, as golden
         * idx 5990 did (spec sec 4k step 2), so an established VAX1 that is
         * waiting for the joiner to initiate verifies both directions. ONE
         * frame, guarded by padded_initiated -- not a flood. */
        if (ps->channel_up && !ps->padded_initiated) {
            size_t plen = 0;
            rx->hello_params->timer_tick = hello_timer_tick100(); /* vms-9f3: live 100ns tick */
            /* abs-92 = SCS_HELLO_JOINER_INCARNATION (1): OVMX's own padded
             * HELLO carries the incarnation it attributes to the peer on a
             * fresh first contact, NOT the member's advertised value (spec
             * sec 4i.B; golden pair carries abs-92 = 1). */
            /* vms-760: SCA dest-logical (abs 16) = the PEER's logical addr
             * from abs 24 of the probe, not its HW MAC (see scs_hello.h). */
            memcpy(rx->hello_params->peer_logical, buf + 24, 6);
            if (scs_hello_build_padded_directed_frame(rx->hello_params, src_mac, rx->lab_nonce,
                                                      SCS_HELLO_JOINER_INCARNATION,
                                                      (uint16_t)SCS_HELLO_PADDED_MAX_SCA,
                                                      rx->pframe, sizeof(rx->pframe), &plen) == 0 &&
                send_frame_channel(rx->sock, rx->ifindex, src_mac, rx->pframe, plen) > 0) {
                ps->padded_initiated = 1;
                clock_gettime(CLOCK_MONOTONIC, &ps->last_padded);
                rx->padded_sent++;
                log_ts(stdout);
                printf(" SCSD-I-PADINIT, initiated padded HELLO (%u SCA / %zu wire)"
                       " to %02x:%02x:%02x:%02x:%02x:%02x (golden joiner-first)\n",
                       (unsigned)SCS_HELLO_PADDED_MAX_SCA, plen,
                       src_mac[0], src_mac[1], src_mac[2],
                       src_mac[3], src_mac[4], src_mac[5]);
                fflush(stdout);
            }
        }

        /* vms-d94: OVMX is a cluster JOINER, and a real joiner ACTIVELY OPENS
         * its OWN VMS$VAXcluster connection to the member and sends its
         * add-member request on it -- it is NOT a pure passive responder.
         * GROUNDED on the clean 2-node reference (formation-clean-2node.pcap):
         * the joiner VAXB, after the START handshake + directory exchange,
         * sends its OWN VMS$VAXcluster CONNECT-REQUEST (idx52, remote=0,
         * offering its own Con.ID) and then streams the 190-byte add-member
         * config on THAT joiner-initiated connection (idx59). The prior
         * vms-246 "pure joiner-responder" derivation was wrong: it saw only
         * the member->joiner connect in formation-ci1 and missed that the
         * joiner ALSO opens its own connection (VMS$VAXcluster is symmetric).
         * OVMX answering the member's connect but sending its burst on the
         * MEMBER-opened connection is exactly why VAX1 ignored the burst and
         * re-issued START round-0 forever.
         *
         * The PROMPT send happens in the directory handler the moment the
         * post-START directory phase begins (see SCSD-I-DIRCONN). Here we
         * only RETRANSMIT it (reusing the same send_seq) every ~1s until the
         * member accepts (joiner_connected), in case the first was lost. */

        /* vms-578 INTEGRATION: the two retransmit drivers below are BOTH kept.
         * The first is the closure branch's (vms-398/vms-abc) -- it retransmits
         * ONLY the VMS$VAXcluster CONNECT-REQUEST, and it now also requires
         * join_step == JS_IDLE so it cannot double-send while the worktree-760
         * sequencer owns that step. The second is the worktree-760 join
         * SEQUENCER, which retransmits whichever step is outstanding. On the
         * pure-server path join_step stays JS_IDLE and only the first runs; on
         * the sequencer path only the second runs. Neither is reachable from the
         * other's configuration, which is why keeping both is not a duplicate.
         *
         * vms-694: this used to be the ONLY place either driver ran, which made
         * both "coarsely HELLO-driven" -- see scsd_join_retx_for_peer()'s header
         * for why a live lab-2 rejoin showed that starving OVMX's own retransmit
         * silent with retry headroom left. The logic is unchanged here (same
         * function, same call), and scsd_join_retx_tick() now ALSO drives it
         * from the main loop's own clock. */
        scsd_join_retx_for_peer(rx, ps, (long)monotonic_ms());

        /* vms-e81: the newcomer greet timer. A member opens its OWN
         * SCS$DIRECTORY (then MSCP$DISK) to a node that joined after us,
         * driven by a periodic scan rather than by an inbound frame --
         * grounded on control-member-meets-late-node-20260730.pcap, where
         * each node opens whatever it lacks on its own ~1 s phase and the
         * only prerequisite is a completed 0x41 START handshake. */
        if (rx->do_connect && ps->greet_due_ms != 0 &&
            (long)monotonic_ms() >= ps->greet_due_ms &&
            ps->join_step == JS_IDLE && !ps->own_dir_sent &&
            !cm_lean_vc_suppress_peer(rx->peers, ps) && /* vms-9af: lean coordinator VC */
            !cm_rejoin_lean_early_hold(rx->peers, ps, (long)monotonic_ms())) {
            /* vms-3aba: hold own-dir on an appears-first candidate coordinator
             * until the topology settles, so lean-VC engages BEFORE op 0x02 */
            ps->greet_due_ms = 0;
            if (send_own_dir_connect_request(rx->sock, (int)rx->ifindex, ps,
                                             rx->our_hw_mac, rx->our_src_logical)) {
                ps->join_step = JS_DIR_CONNECT;
                clock_gettime(CLOCK_MONOTONIC, &ps->js_last_tx);
                log_ts(stdout);
                printf(" SCSD-I-MEMBGREET, opened OUR SCS$DIRECTORY to the"
                       " newcomer -- member client half begins\n");
                fflush(stdout);
            }
        }

        /* vms-760: emit the DEFERRED step-1 barrier request once the
         * coordinator has had time to finish fanning op 0x0a out to the
         * other members. Sending it from the receive path (~20 us) is what
         * got it acked 0x0260 and never counted, freezing the whole
         * cluster's transition -- 6/6 across every 3-member run. */
        if (ps->barrier_go_pending &&
            (monotonic_ms() - ps->barrier_go_ms) >= (long)JOIN_BARRIER_GO_DELAY_MS) {
            ps->barrier_go_pending = 0;
            cm_send_barrier_step(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                 rx->our_src_logical, ps->barrier_step);
        }

        /* vms-760: FLUSH a residual cat-0x04 ack backlog. The receive path
         * batches at SCS_CM_ACK_EVERY, which by itself strands the last one or
         * two messages of a burst below the threshold -- the coordinator then
         * waits forever for an ack it will never get (d94-e6: acked through 76,
         * messages 77-78 arrived, transaction stopped). Batching plus this
         * flush gives the reference's frame economy without the stall. */
        if (rx->do_connect && ps->cm_local_conid != 0 &&
            ps->sysap_recv != ps->sysap_acked &&
            !rejoin_hold_standalone_ack(ps) && /* vms-71d */
            (monotonic_ms() - ps->cm_last_ack_ms) >= (long)SCS_CM_ACK_FLUSH_MS) {
            cm_send_ack(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac, rx->our_src_logical);
        }

        /* vms-760: send the DEFERRED third config message -- category 0x01
         * opcode 0x02 (config/topology) -- on OUR joiner VC, a few seconds
         * after the MODEL+PARAMS pair.
         * GROUNDED, vax3-2to3-established-join-20260730.pcap: the joiner's
         * initial burst is MODEL+PARAMS only (frames 142/143, +0.9394) and
         * the peer reciprocates in kind; the joiner then sends op 0x02 LATER
         * (frame 285, +5.8774, smsg=3 amsg=2) and THAT is what starts the
         * admission transaction -- the peer answers 0x04/0x00 within 0.3 ms
         * (287) and immediately drives op 0x03 COMMIT (291) -> op 0x05 lock
         * rebuilds (293/294/297/299) -> MEMBER. Deferring it FOREVER (the
         * previous behaviour) leaves the dialogue permanently half-finished.
         * The ~5 s delay is taken from the reference; what the real joiner
         * actually waits on is NOT grounded (see spec 5(z)). */
        /* vms-760: the candidate set must SETTLE before we pick.
         * coord9: the timer fired 4.9 s after VAX2's config while VAX3 --
         * the actual coordinator -- had not yet exchanged config with us at
         * all. cm_pick_coordinator() correctly returned the highest peer it
         * could see, which was the wrong node, and VAX2 acked the op 0x02
         * without proposing anything. Picking early is the same bug as
         * picking by the wrong rule.
         * So: wait for the delay to elapse since the LAST peer configured,
         * and, while any peer we have an open channel to has still not
         * configured, keep waiting -- bounded, so a permanently silent peer
         * cannot block the join forever. */
        /* vms-4838: on a REJOIN, drive the add-member config burst PROACTIVELY
         * on the MEMBER-INITIATED VMS$VAXcluster connection the moment it
         * reaches OPEN -- do NOT wait for a member CM config frame the way the
         * server-first trigger (SCSD-I-CMCONFIG) does. GROUNDED (spec 4(O.11),
         * SUCCESS oracle): the rejoiner (Figure-2-14 TARGET) sends op 0x02
         * (f1297) FIRST, and only THEN does the member reciprocate (cat 0x04 op
         * 0x04, f1299). On a rejoin the member is waiting for us; if OVMX also
         * waits for the member the readmission deadlocks (arm C/D, XITDONE=0).
         *
         * The burst rides the member CDT's Con.ID pair (PS_LOCAL_CONID(ps) /
         * ps->remote_conid, the same pair the reactive trigger uses), so
         * cm_send_config_burst() stamps cfg_local_conid/cfg_remote_conid to the
         * member connection and the DEFERRED op 0x02 below rides it too. The
         * frame CONTENTS are unchanged from what OVMX already sends -- this is a
         * connection-SELECTION fix, not a payload change (spec 4(O.11)). */
        if (cm_rejoin_target_mode() && rx->do_connect &&
            !ps->appeared_after_join && !ps->cm_config_sent &&
            ps->cdt_member != NULL &&
            scs_conn_state_of(ps->cdt_member) == SCS_CONN_OPEN) {
            int c = cm_send_config_burst(rx->sock, (int)rx->ifindex, ps,
                                         rx->our_hw_mac, rx->our_src_logical,
                                         PS_LOCAL_CONID(ps), ps->remote_conid);
            rx->cm_config_frames += c;
            log_ts(stdout);
            printf(" SCSD-I-CMREADMIT, REJOIN: drove add-member burst (%d frames)"
                   " PROACTIVELY on the MEMBER-INITIATED VMS$VAXcluster connection"
                   " local=0x%08X remote=0x%08X (Figure-2-14 TARGET, spec 4(O.11))"
                   " -- deferred op 0x02 + reciprocation follow on THIS conid\n",
                   c, (unsigned)PS_LOCAL_CONID(ps), ps->remote_conid);
            fflush(stdout);
        }

        long cfg_settle_ms = 0;
        int cfg_waiting_on_peer = 0;
        for (int pi = 0; pi < OVMX_MAX_PEERS; pi++) {
            if (!(rx->peers[pi].pb != NULL)) {
                continue;
            }
            if (rx->peers[pi].cfg_sent && rx->peers[pi].cfg_ms > cfg_settle_ms) {
                cfg_settle_ms = rx->peers[pi].cfg_ms;
            }
            if (rx->peers[pi].channel_up && !rx->peers[pi].cfg_sent) {
                cfg_waiting_on_peer = 1;
            }
        }
        if (cfg_waiting_on_peer && cfg_settle_ms != 0 &&
            (monotonic_ms() - cfg_settle_ms) < (long)JOIN_CFG2_MAX_WAIT_MS) {
            cfg_settle_ms = monotonic_ms(); /* hold the timer open */
        }
        /* vms-46f (spec 4(O.17)): THE root-cause fix. Before the deferred op 0x02
         * rides the wire on a REJOIN, INITIATE OVMX's op-6 special-credit request
         * on the coordinator's SCS$DIRECTORY connection and RETURN Pending Receive
         * Credit -- mirroring the SUCCESS oracle's f1261(ss=13) op-6 ahead of
         * f1297(ss=14) op 0x02. Without this the coordinator stays SEND-CREDIT
         * STARVED (Davis pp. 2-43/2-45), cannot transmit its op 0x04 reciprocation
         * (Credit Wait), and returns cm_responses=0 / XITDONE=0. The op 0x02 gate
         * below additionally REQUIRES this latch on the fix path, so op-6 provably
         * precedes op 0x02. Kill-switch OVMX_REJOIN_CREDIT_FIRST=0 restores the
         * pre-fix order. Requires the coordinator to have opened its SCS$DIRECTORY
         * connection (dir_remote_conid learned) -- grounded present on a rejoin
         * (spec 4(O.17): OVMX f60 op=9 on 0x0C640007/0x8DE4000D). */
        if (rejoin_credit_first_enabled() && rx->do_connect &&
            !ps->appeared_after_join && ps->cfg_sent &&
            !ps->joiner_cfg2_sent && !ps->rejoin_credit_first_sent &&
            ps->dir_remote_conid != 0) {
            /* p. 2-43/2-44: hand the received directory buffer back as Pending
             * Receive Credit, then return it on the wire ahead of op 0x02 so the
             * 190-byte op 0x02 (a credit class) piggybacks it at abs[48:50]. */
            if (ps->cdt_dir != NULL) {
                (void)scs_credit_release_buffer(ps->cdt_dir);
            }
            if (scs_send_rejoin_credit_op6(rx->sock, (int)rx->ifindex, ps,
                                           rx->our_hw_mac, rx->our_src_logical)) {
                ps->rejoin_credit_first_sent = 1;
                rx->cm_config_frames++;
                log_ts(stdout);
                printf(" SCSD-I-CREDITFIRST, REJOIN: initiated op-6 special-credit"
                       " request on the coordinator's SCS$DIRECTORY connection"
                       " (local=0x%08X remote=0x%08X) BEFORE op 0x02 -- returns"
                       " Pending Receive Credit so the coordinator can send its"
                       " op 0x04 reciprocation (spec 4(O.17), Davis pp. 2-43/2-44)\n",
                       (unsigned)PS_SCS_DIR_CONID(ps),
                       (unsigned)ps->dir_remote_conid);
                fflush(stdout);
            }
        }

        /* vms-e81: op 0x02 is the JOINER's add-member request. A member never
         * sends one -- in neither pure-VMS control does VAX1 or VAX2 ever send
         * op 0x02 to the newcomer. We were sending it to a node that was not
         * yet a member: an established member asking a newcomer to admit IT.
         * Together with the joiner-form op 0x01 that told the newcomer twice,
         * by two mechanisms, that we were trying to join. */
        /* vms-46f: on the fix path (rejoin + kill-switch on), HOLD op 0x02 until
         * the op-6 credit round has ridden the wire -- this is what makes op-6
         * provably precede op 0x02 (spec 4(O.17)). Off the fix path the extra
         * clause is a no-op, so the first-join and kill-switch orders are
         * unchanged. */
        if (rx->do_connect && !ps->appeared_after_join &&
            ps->cfg_sent && !ps->joiner_cfg2_sent &&
            (getenv("OVMX_CFG2_ALL") != NULL ||
             cm_pick_coordinator(rx->peers) == ps) &&
            (monotonic_ms() - cfg_settle_ms) >= (long)JOIN_CFG2_DELAY_MS &&
            (!rejoin_credit_first_enabled() || ps->rejoin_credit_first_sent)) {
            struct scs_member_params mp;
            memset(&mp, 0, sizeof(mp));
            memcpy(mp.dst_mac, ps_port_addr(ps), 6);
            memcpy(mp.src_mac, rx->our_hw_mac, 6);
            memcpy(mp.src_logical, rx->our_src_logical, 6);
            memcpy(mp.peer_logical, ps_sys_addr(ps), 6);
            mp.remote_conid = ps->cfg_remote_conid;
            mp.local_conid = ps->cfg_local_conid;
            mp.incarnation = ps->incarnation;
            mp.recv_ack = ps->vc.seq.recv_seq;
            mp.send_seq = scs_seq_advance(&ps->vc.seq);
            mp.sysap_send_msg = ps->sysap_send++;
            mp.sysap_ack_msg = ps->sysap_recv; /* ref frame 285: amsg=2 = peer's smsg */
            cm_apply_rejoin_form(&mp); /* vms-2f3 */
            uint8_t c2frame[SCS_MEMBER_FRAME_LEN];
            if (scs_member_build_config(&mp, c2frame) == 0 &&
                send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                                  "deferred add-member op 0x02 (config/topology)",
                                  c2frame,
                              sizeof(c2frame)) > 0) {
                scs_vc_record_sent(&ps->vc, mp.send_seq, monotonic_ms());
                ps->joiner_cfg2_sent = 1;
                rx->cm_config_frames++;
                log_ts(stdout);
                printf(" SCSD-I-CMCONFIG2, sent DEFERRED op 0x02 config/topology"
                       " to node %u on the %s VC local=0x%08X remote=0x%08X"
                       " (send_msg=%u ack_msg=%u) -- expect its 0x04 ack then its"
                       " op 0x03 COMMIT\n",
                       (unsigned)(peer_node_number(ps) & 0x03ff),
                       cm_rejoin_target_mode() ? "MEMBER-initiated (rejoin)"
                                               : "OUR joiner",
                       (unsigned)ps->cfg_local_conid, (unsigned)ps->cfg_remote_conid,
                       mp.sysap_send_msg, mp.sysap_ack_msg);
                fflush(stdout);
            }
        }

        /* vms-760 PURE-SERVER disk-client: stop-and-wait retransmit of the
         * current outstanding PS drive frame if VAX1's response has not
         * arrived. Reuses the stored send_seq (never advances -- 760mscp
         * hole). Only the connect/lookup steps are retransmitted here; the
         * happy path advances receive-driven and never needs this. */
        if (getenv("OVMX_PURE_SERVER") != NULL && ps->psc_step != PSC_IDLE &&
            ps->psc_step != PSC_DONE && ps->psc_retx < JOIN_RETX_MAX) {
            long now_ms = monotonic_ms();
            long last_ms = ps->psc_last_tx.tv_sec * 1000L +
                           ps->psc_last_tx.tv_nsec / 1000000L;
            /* vms-694: PSC_MSCP_CONNECT is the SAME MSCP$DISK CONNECT-REQUEST
             * wire message as JS_MSCP_CONNECT above, so it rides the same
             * grounded ~10s cadence; the other PS steps keep the generic
             * JOIN_RETX_TIMEOUT_MS. */
            long step_timeout_ms = (ps->psc_step == PSC_MSCP_CONNECT)
                                        ? mscp_connect_retx_timeout_ms()
                                        : (long)JOIN_RETX_TIMEOUT_MS;
            if ((now_ms - last_ms) >= step_timeout_ms) {
                int sent = 0;
                switch (ps->psc_step) {
                case PSC_DIR_CONNECT:
                    sent = ps_send_dir_connect(rx->sock, (int)rx->ifindex, ps,
                                               rx->our_hw_mac, rx->our_src_logical);
                    break;
                case PSC_DIR_LOOKUP_TAPE:
                case PSC_DIR_LOOKUP_DISK:
                    sent = ps_send_dir_lookup(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                              rx->our_src_logical, NULL, 1);
                    break;
                case PSC_MSCP_CONNECT:
                    sent = ps_send_mscp_connect(rx->sock, (int)rx->ifindex, ps,
                                                rx->our_hw_mac, rx->our_src_logical);
                    break;
                default:
                    break; /* SCC/GUS in flight advance receive-driven */
                }
                if (sent) {
                    ps->psc_retx++;
                    log_ts(stdout);
                    printf(" SCSD-I-PSCLIENT, retransmit disk-discovery step %d"
                           " (retx %u)\n", ps->psc_step, ps->psc_retx);
                    fflush(stdout);
                }
            }
        }
        return;
    }

    /* (b) vms-21e: phase-2 START/config (opcode 0x41). After the channel
     * forms, the established node streams 0x41 START frames and WAITS for
     * OVMX's own 0x41 START before proceeding to the 0x4b connect. Answer
     * as the joiner does (spec sec 4g phase 2): reply round-0 + round-1
     * START, then the round-2 46-byte ack when the peer acks. */
    if (rx->do_connect && n >= 32 && buf[30] == SCS_START_OPCODE) {
        struct scs_start_view sv;
        if (scs_start_parse(buf, (size_t)n, &sv) != 0) {
            return;
        }
        struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
        if (ps == NULL) {
            return;
        }
        /* vms-578: worktree-760's vms-e81 RESTART detection, kept verbatim.
         * It runs BEFORE scs_vc_init() because a genuine re-START must clear
         * the session latches first; the vms-4071 formation FSM below then
         * re-runs the dialogue from CLOSED exactly as on a fresh contact. */
        /* vms-e81: A PEER MAY START US AGAIN. These were one-shot latches
         * that nothing ever cleared, so a peer could complete the 0x41
         * handshake with a given OVMX peer-slot exactly ONCE per process
         * lifetime. VMS re-STARTs routinely -- when VAX3 decided our VC was
         * dead it re-STARTed 55 times over four minutes and OVMX parsed,
         * logged and SILENTLY DROPPED every one (105 STARTRX lines with no
         * STARTTX). A silent drop of a message the peer is waiting on is the
         * Rule 9 / INV-6 failure shape exactly: it reports nothing wrong
         * while making the peer unreachable forever.
         *
         * A round-0 START from a peer we have already handshaked is a
         * RESTART. Tear the slot's session state down and run the handshake
         * again as if new. Do NOT gate this on the peer's send_seq being 1 --
         * spec 4i.A grounds that an established peer's round-0 START
         * legitimately carries a large residual send_seq (10 here, 11974 in
         * the reference). */
        /* REGRESSION GUARD, learned the hard way one run later: the normal
         * handshake is MULTI-ROUND. VMS sends round-0 and round-1 START
         * back-to-back and BOTH are non-ack, so 'non-ack START from a peer
         * we have already replied to' matches the ordinary second round.
         * The first version of this check fired on it, wiped the session,
         * and looped -- 2883 'restarts' in one run and OVMX never completed
         * even its own join. A repair that fires during normal operation is
         * worse than the fault it repairs.
         *
         * A genuine restart is distinguished by TIME, not by round: the peer
         * has been quiet on this channel, decided it was dead, and begun
         * again. Require the handshake to have fully completed (start_acked)
         * and a quiet gap since the last START we processed. */
        long start_now_ms = monotonic_ms();
        if (!sv.is_ack && ps->start_acked &&
            (start_now_ms - ps->last_start_ms) >= (long)START_RESTART_GAP_MS) {
            log_ts(stdout);
            printf(" SCSD-I-RESTART, peer re-STARTed an established channel"
                   " (peer_seq=%u) -- resetting this peer's session and"
                   " re-running the handshake\n",
                   (unsigned)sv.send_seq);
            fflush(stdout);
            ps->start_replied = 0;
            ps->start_acked = 0;
            ps->vc.initialized = 0;
            /* Drop the SYSAP state bound to the dead VC; it is all invalid
             * now and stale handles are what a peer rejects. */
            ps->dir_connected = 0; ps->dir_remote_conid = 0; ps->dir_seen = 0;
            ps->own_dir_sent = 0; ps->own_dir_connected = 0;
            ps->own_dir_lookup_sent = 0;
            ps->lean_vc_suppressed = 0;   /* vms-9af/3aba: re-decide lean-VC on the */
            ps->lean_hold_start_ms = 0;   /* re-formed VC; restart the settle window */
            ps->mscp_connect_sent = 0; ps->mscp_connected = 0;
            ps->connected = 0; ps->connect_sent = 0; ps->remote_conid = 0;
            ps->joiner_connect_sent = 0; ps->joiner_connected = 0;
            /* vms-694 (§4(O.7)): a re-START is a genuine loss of THIS membership
             * incarnation -- the VMS$VAXcluster connection must be re-formed and
             * re-reach OPEN before OVMX may again model itself as a member. This
             * is NOT the graceful-shutdown teardown (which must keep the latch);
             * it is the peer tearing the session down, so drop the honest fact. */
            ps->vaxcluster_open_reached = 0;
            ps->cm_config_sent = 0; ps->cfg_sent = 0;
            ps->rejoin_credit_first_sent = 0; /* vms-46f: re-run the op-6 credit round
                                               * when the VC re-forms (spec 4(O.17)) */
            ps->join_step = JS_IDLE; ps->js_retx = 0;
            ps->greet_due_ms = 0;
            /* vms-578: re-arm the vms-4071 formation state machine too.
             * Its state lives in the Path Block (p. 2-11), which
             * worktree-760 predates; without this the circuit would stay
             * OPEN in the FSM's view across a re-START. */
            scs_vc_fsm_init(ps->pb);
            scs_pb_set_vc_state(ps->pb, SCS_VC_CLOSED);
        }
        ps->last_start_ms = start_now_ms;
        if (!ps->vc.initialized) {
            scs_vc_init(&ps->vc);
        }
        /* vms-246: the 0x41 START handshake is driven by the config-round
         * (spec sec 4g/4i.A), NOT by the SCS VC sequence space. Do NOT fold
         * the member's START send_seq into the VC recv_seq -- the member's
         * round-0 START may carry a large residual send_seq (sec 4i.A) and
         * the post-START VC resets to send_seq=1/recv_seq=0 on BOTH sides.
         * Feeding START counters into the VC left OVMX acking a sequence the
         * VAX never sent post-reset, so the VAX rejected OVMX's directory
         * CONNECT-RESPONSE and retransmitted its 0x5b/0x7b request forever. */
        /* vms-7be: a START describes the sending node, and its description is
         * what the System Block is built from (p. 2-12). OVMX fills the SB
         * with the one item its START parser resolves today -- the 48-bit SCS
         * System Address -- and leaves the CPU type / hardware revision / OS
         * name+version / 64-bit software incarnation fields unset rather than
         * inventing them. (The 16-bit node-incarnation OVMX echoes on the wire
         * is a NISCA field, NOT the 64-bit SCA software incarnation number of
         * p. 2-16; conflating them would be a fabrication.) */
        ps_learn_sys_addr(rx->cfg, ps, buf + OFF_HELLO_SRCLOG); /* START src-logical, abs 24 */

        log_ts(stdout);
        printf(" SCSD-I-STARTRX, %s round=%u peer_seq=%u peer_sysid=%u\n",
               sv.is_ack ? "ack" : "START", sv.config_round, sv.send_seq,
               sv.is_ack ? 0 : sv.scssystemid);
        fflush(stdout);

        /* ===== vms-4071: drive the VC FORMATION state machine =====
         *
         * This replaces the pair of unconditional scs_pb_set_vc_state()
         * writes that used to stand in for the dialogue. Every transition
         * now comes from scs_vc_fsm_recv()'s implementation of the p. 2-14
         * acceptable-response table, and OVMX's emissions are whatever
         * action the machine returns. See the scsd_vc_* helpers above for
         * the config-round <-> START/STACK/ACK mapping and for the two
         * declared wire-visible differences. */
        uint64_t vc_now_ms = monotonic_ms();
        enum scs_vc_event vc_ev = scs_vc_classify_round(sv.is_ack, sv.config_round);

        /* ---- PHASE 1: TRANSMIT. NOTHING BETWEEN THE TWO SENDS. ----
         * The pre-vms-4071 code emitted round-0 and round-1 from a two-pass
         * `for` loop with nothing at all between the sendto() calls. An
         * earlier revision of this block put a log_ts+printf+fflush after
         * EACH send, which measurably widened that gap: of four lab-2 runs
         * of that revision (vaxlab-3, 2026-08-03, tags W1A/W1B/W5A/W6A) one
         * -- W6A -- captured the peer's round-1 arriving BETWEEN OVMX's
         * round-0 and round-1; neither pre-vms-4071 control run (W4A, W7A)
         * showed that, both keeping the two sends adjacent. Same frames and
         * the same order FROM OVMX either way, but a wider window is still a
         * wire-timing change, so the logging is deferred to phase 2 below.
         * Re-measured after the split: W8A is adjacent again.
         *
         * NOT UNIT-TESTED, and it cannot be through the SCSD_UNIT_TEST seam:
         * this code is inside main()'s receive loop, which that seam renames
         * away. tests/vmsscs/test_scsd_wire.c covers the helpers this block
         * calls; the adjacency itself is held only by the lab captures named
         * above. */
        int sent_start = 0;
        int sent_stack = 0;
        enum scs_vc_state state_after_start = ps->pb->vc_state;

        /* CLOSED -> START SENT. Trigger deliberately unchanged from vms-21e:
         * OVMX issues its own START only when a peer identity-bearing frame
         * arrives, never off a bare 46-byte ack (an ACK in START SENT is an
         * unacceptable response and would abandon the circuit at once). */
        if (vc_ev != SCS_VC_EV_ACK && ps->pb->vc_state == SCS_VC_CLOSED &&
            !ps->pb->fsm.abandoned) {
            if (scs_vc_fsm_send_start(ps->pb, vc_now_ms) == SCS_VC_ACT_SEND_START &&
                scsd_vc_emit(rx->vc_ctx, ps, SCS_VC_ACT_SEND_START)) {
                rx->start_sent++;
                ps->start_replies++;
                ps->start_replied = 1;
                sent_start = 1;
                state_after_start = ps->pb->vc_state;
            }
        }

        enum scs_vc_action vc_act = scs_vc_fsm_recv(ps->pb, vc_ev, vc_now_ms);

        if (vc_act == SCS_VC_ACT_SEND_STACK) {
            if (scsd_vc_emit(rx->vc_ctx, ps, SCS_VC_ACT_SEND_STACK)) {
                rx->start_sent++;
                ps->start_replies++;
                sent_stack = 1;
            }
        }

        /* ---- PHASE 2: LOG. Nothing below touches the wire. ---- */
        if (sent_start) {
            log_ts(stdout);
            /* sys_incarnation is the [66:74] quadword scsd_vc_emit() just
             * stamped (the accessor caches, so this is the same value that
             * went out). 0 means OVMX_INCARNATION_FROZEN=1 -- the control arm,
             * shipping the replayed template bytes. Logged so a lab run can be
             * checked without a capture. */
            printf(" SCSD-I-STARTTX, sent round-0 START (VC %s)"
                   " (sysid=%u node='%s' send_seq=%u recv_ack=0 incarnation=%u"
                   " sys_incarnation=0x%016llx)\n",
                   scs_vc_state_name(state_after_start), rx->vc_ctx->scssystemid, rx->vc_ctx->node_name,
                   ps->vc.seq.send_seq, ps->incarnation ? ps->incarnation : 1,
                   (unsigned long long)ovmx_incarnation_time());
            fflush(stdout);
        }
        log_ts(stdout);
        printf(" SCSD-I-VCFSM, %s received -> %s, VC %s\n",
               scs_vc_event_name(vc_ev), scs_vc_action_name(vc_act),
               scs_vc_state_name(ps->pb->vc_state));
        fflush(stdout);
        if (sent_stack) {
            log_ts(stdout);
            printf(" SCSD-I-STARTTX, sent round-1 STACK (VC %s)\n",
                   scs_vc_state_name(ps->pb->vc_state));
            fflush(stdout);
        }
        /* OVMX's round-2 ack is emitted by scsd_vc_settle(), exactly once
         * per circuit, followed by the p. 2-21 open transition. By DEFAULT
         * the trigger is the third argument below -- "the peer's round-2
         * 46-byte frame is what we just processed" -- which is the identical
         * trigger the pre-vms-4071 `else if (sv.is_ack)` branch used, so the
         * fresh-join interleaving is unchanged. OVMX_VC_EARLY_ACK=1 moves it
         * to the OPEN transition instead (scsd_vc_ack_due). */
        int was_acked = ps->start_acked;
        scsd_vc_settle(rx->vc_ctx, ps, vc_act, scsd_vc_peer_round2(vc_ev),
                       &rx->start_ack_sent);

        /* vms-578 INTEGRATION -- THE JOIN SEQUENCER'S IGNITION.
         *
         * On worktree-760 this hung off `if (!ps->start_acked) { ... }` inside a
         * hand-rolled START responder that the vms-4071 formation FSM replaced.
         * The FSM owns the round-2 ack now (scsd_vc_settle -> scsd_vc_on_open),
         * so the trigger is re-expressed as the START_ACKED EDGE: false before
         * settle, true after. That is the same instant -- scsd_vc_settle sets
         * start_acked in exactly one place and calls scsd_vc_on_open() from it --
         * so the sequencer starts on the frame it started on before.
         *
         * WHAT IS NOT CLAIMED: nothing here has been measured to fire in the
         * same wire position as worktree-760's copy. The equivalence above is
         * read off the two control flows, not off a capture. */
        if (!was_acked && ps->start_acked) {
        fflush(stdout);
        /* vms-760: KICK OFF THE JOIN SEQUENCER. Open OUR OWN
         * SCS$DIRECTORY client connection as step 1 of the full
         * dir-client choreography (docs/design-cluster-join-
         * choreography.md). The earlier 760b failure -- opening our
         * own dir then jumping straight to the VC connect -- was an
         * OVMX DRIVE bug (VC issued before its SYSAP was resolved),
         * NOT a protocol block: the clean member opens its own dir
         * probe regardless (formation-clean-2node SCA idx76) while
         * the joiner is a dir-client. The sequencer now drives the
         * connects strictly IN ORDER, each gated on its lookup HIT,
         * so no unprocessable frame ever enters the shared seq. The
         * member-opened dir probe is still answered by branch b2
         * (dir-SERVER) on a distinct Con.ID pair. */
        /* vms-e81: DO NOT run the joiner sequence at a peer that
         * appeared AFTER we became a member.
         *
         * Live bystander test: OVMX joined a 2-node cluster, then
         * VAX3 booted into it. OVMX saw a new peer and started its
         * JOINER choreography AT VAX3 -- which was itself mid-join
         * and never answered -- retransmitting join step 4 at a
         * node that was in no position to reply. VAX3 stalled at
         * NEW, OVMX went BRK_NON, and OVMX was never invited into
         * the transition because it had formed no SCS connection
         * with the newcomer at all.
         *
         * Joining is something you do ONCE, to a cluster that
         * already exists. Meeting a node that arrives later is the
         * MEMBER role, and its obligations are different: accept
         * the newcomer's connects, open your own SCS$DIRECTORY and
         * MSCP$DISK client to it, and open a VMS$VAXcluster VC only
         * if it has not already opened one to you (grounded from
         * VAX1's side of vax3-2to3-established-join). Running
         * admission at it is not merely useless -- it is noise
         * aimed at a node in the middle of its own join.
         *
         * MEMBER-SIDE ORDERING -- CORRECTED, and the earlier
         * reading was BACKWARDS.
         *
         * I first concluded "the member does not move first",
         * from VAX1's leg of vax3-2to3 where VAX1 accepted VAX3's
         * connects and opened its own ~17 ms later. That leg is
         * joiner-first only because the joiner's scan happened to
         * fire first. THE SAME CAPTURE's VAX2 leg is MEMBER-FIRST
         * by 460 ms: VAX2 opened SCS$DIRECTORY (f190),
         * VMS$VAXcluster (f208) and MSCP$DISK (f223) to VAX3 when
         * VAX3 had sent it NOTHING but the HELLO/START handshake
         * -- no connect, no lookup, no config. Generalising from
         * one leg of one capture is the same error as reading
         * op 0x12's body[20:24] as a member count.
         *
         * A dedicated pure-VMS control settles it
         * (control-member-meets-late-node-20260730.pcap, VAX1+VAX2
         * live, then VAX3 boots in, no OVMX present): BOTH SIDES
         * INITIATE INDEPENDENTLY. Each node runs its own periodic
         * scan (~1 s) and opens whatever it lacks; the two
         * connection sets are separate Con.ID pairs and both are
         * expected to exist. The ONLY trigger is the completed
         * 0x41 START handshake -- in the reference the member's
         * connect follows its own START-ack with no intervening
         * frame in either direction.
         *
         * Waiting on dir_seen therefore DEADLOCKED us: a newcomer
         * whose own scan has not yet reached OVMX sees no
         * connection, so OVMX is correctly excluded from the
         * transition -- exactly what run by3 showed. We now
         * initiate once START is complete, symmetric with the
         * joiner. */
        if (rx->join_seq_enabled && ps->appeared_after_join &&
            ps->greet_due_ms == 0) {
            /* NEWCOMER: arm the greet timer instead of sending
             * from here. We are inside the !start_acked branch,
             * so start_acked is still false at this point --
             * testing it here could never fire, which is exactly
             * why run by4 showed START complete and connect_sent=0. */
            ps->greet_due_ms = monotonic_ms() + JOIN_MEMBER_GREET_MS;
            log_ts(stdout);
            printf(" SCSD-I-MEMBGREET, newcomer START complete --"
                   " opening our client half in %u ms\n",
                   (unsigned)JOIN_MEMBER_GREET_MS);
            fflush(stdout);
        }
        if (rx->join_seq_enabled && !ps->appeared_after_join &&
            ps->join_step == JS_IDLE && !ps->own_dir_sent &&
            !cm_lean_vc_suppress_peer(rx->peers, ps) && /* vms-9af: lean coordinator VC */
            !cm_rejoin_lean_early_hold(rx->peers, ps, (long)monotonic_ms()) && /* vms-3aba */
            send_own_dir_connect_request(rx->sock, (int)rx->ifindex, ps,
                                         rx->our_hw_mac, rx->our_src_logical)) {
            ps->join_step = JS_DIR_CONNECT;
            clock_gettime(CLOCK_MONOTONIC, &ps->js_last_tx);
            log_ts(stdout);
            printf(" SCSD-I-JOINSEQ, opened OUR SCS$DIRECTORY client"
                   " connect local=0x%08X seq=%u (join step 1/8)\n",
                   (unsigned)PS_SCS_DIR_JOINER_CONID(ps), ps->own_dir_req_seq);
            fflush(stdout);
        }
        }

        /* vms-694: NAME THE "peer withholds its round-2 ACK" STALL rather than
         * loop on it silently. The circuit is OPEN on our side, but the peer keeps
         * re-issuing round-0 START and never sends its round-2 ACK, so start_acked
         * never latches and the join sequencer above never ignites. GROUNDED live
         * (2026-08-09, lab-2): a returning/duplicate SCSSYSTEMID is refused by the
         * member exactly this way (spec sec 4(w)) -- the clean-identity control on
         * the same build joined at t+13 s with the member's round-2 ACK present. A
         * silent loop here is the INV-6 shape: it made a §4(w) identity conflict
         * read as a VC-formation regression. Log-only; emitted once per peer. */
        if (!ps->vc_noack_warned &&
            scs_vc_noack_stall(ps->pb, ps->start_acked,
                               SCS_VC_NOACK_STALL_THRESHOLD)) {
            ps->vc_noack_warned = 1;
            log_ts(stderr);
            fprintf(stderr,
                    " SCSD-W-VCNOACK, virtual circuit OPEN but peer %02x:%02x:%02x:"
                    "%02x:%02x:%02x has re-issued round-0 START %lu time(s) WITHOUT"
                    " sending its round-2 ACK -- our round-2 ACK never went out"
                    " (start_acked=0) and the join sequencer will not ignite. This"
                    " is the admission-refusal signature: most often a returning or"
                    " duplicate SCSSYSTEMID (spec sec 4(w)); check the peer console"
                    " for %%PEA0 'Remote System Conflicts with Known System'. Mint a"
                    " fresh identity on a pod that has never seen it.\n",
                    ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                    ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
                    ps->pb->fsm.start_after_open);
            fflush(stderr);
        }
        return;
    }

    /* (b1) vms-d94: the member ACCEPTS OUR active-joiner VMS$VAXcluster
     * CONNECT-REQUEST. GROUNDED on the live wire (d94-fix2.pcap idx34): once
     * OVMX sends its CONNECT-REQUEST promptly after START, the member stops
     * re-issuing START and replies with a frame (observed op 0x5b, the
     * directory/resolution class) whose remote Con.ID == OVMX_JOINER_CONID
     * (our handle, echoed) and whose local Con.ID is the member's own
     * freshly-supplied handle -- i.e. the CONNECT-RESPONSE that binds OUR
     * joiner connection. scs_dir_parse does not classify it (remote != 0, op
     * != 0x0a), so catch it here by the Con.ID signature and drive the
     * add-member burst on the bound joiner VC.
     *
     * The opcode set below deliberately includes the 0x4b SEQAPP class: the
     * member's answer is 0x5b on some runs and 0x4b on others, and BOTH must
     * bind. Because this block returns, it is the ONLY joiner-accept path in
     * the daemon -- branch (c) below carries no duplicate, and the comment
     * there records why re-adding one would be dead code. MEASURED: replaying
     * all 19 OVMX lab captures (141,338 frames) through scsd_handle_frame()
     * runs this bind 39 times and any (c)-side one 0 times. */
    if (rx->do_connect && getenv("OVMX_PURE_SERVER") != NULL && n >= 72 &&
        (buf[30] == SCS_MSGTYPE_SEQAPP || buf[30] == SCS_DIR_OPCODE ||
         buf[30] == SCS_DIR_OPCODE_RETX)) {
        uint32_t ps_rconid = (uint32_t)buf[64] | ((uint32_t)buf[65] << 8) |
                             ((uint32_t)buf[66] << 16) | ((uint32_t)buf[67] << 24);
        uint32_t ps_lconid = (uint32_t)buf[68] | ((uint32_t)buf[69] << 8) |
                             ((uint32_t)buf[70] << 16) | ((uint32_t)buf[71] << 24);
        uint16_t ps_dop = (uint16_t)buf[60] | ((uint16_t)buf[61] << 8);

        /* --- frames on OUR PS SCS$DIRECTORY connection --- */
        if (ovmx_conid_is_class(ps_rconid, OVMX_CONID_CLS_PSDIR) && ps_lconid != 0) {
            struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
            if (ps == NULL) {
                return;
            }
            /* (A) VAX1's op=2 CONNECT-RESPONSE binds OUR dir connection ->
             * send op=3 confirm, then start the SYSAP lookups (MSCP$TAPE
             * first, expected miss; then MSCP$DISK), the choreography VAX1
             * requires before it accepts OUR MSCP$DISK connect. */
            if (!ps->psc_dir_connected && ps_dop == SCS_DIR_OP_RESPONSE) {
                ps->psc_dir_remote_conid = ps_lconid;
                ps->psc_dir_connected = 1;
                ps_send_dir_confirm(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                    rx->our_src_logical);
                ps->psc_step = PSC_DIR_LOOKUP_TAPE;
                ps->psc_retx = 0;
                ps_send_dir_lookup(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                   rx->our_src_logical, "MSCP$TAPE", 0);
                log_ts(stdout);
                printf(" SCSD-I-PSCLIENT, OUR dir bound (remote=0x%08X); confirm +"
                       " lookup MSCP$TAPE seq=%u (disk-discovery step 2-3)\n",
                       ps_lconid, ps->psc_lookup_seq);
                fflush(stdout);
                return;
            }
            /* (B) VAX1's lookup RESPONSE (op=0x0a) advances TAPE->DISK->connect.
             * HIT vs MISS = result [78:94] (abs 92) != "NOT PRESENT HERE". */
            if (ps_dop == SCS_DIR_OP_LOOKUP && n >= 108) {
                int affirmative =
                    memcmp(buf + 92, SCS_DIR_NOT_PRESENT, SCS_DIR_RESULT_LEN) != 0;
                ps->psc_retx = 0;
                if (ps->psc_step == PSC_DIR_LOOKUP_TAPE) {
                    ps->psc_step = PSC_DIR_LOOKUP_DISK;
                    ps_send_dir_lookup(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                       rx->our_src_logical, "MSCP$DISK", 0);
                    log_ts(stdout);
                    printf(" SCSD-I-PSCLIENT, MSCP$TAPE %s; lookup MSCP$DISK"
                           " seq=%u (disk-discovery step 4)\n",
                           affirmative ? "HIT" : "miss", ps->psc_lookup_seq);
                    fflush(stdout);
                } else if (ps->psc_step == PSC_DIR_LOOKUP_DISK && affirmative) {
                    ps->psc_step = PSC_MSCP_CONNECT;
                    ps_send_mscp_connect(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                         rx->our_src_logical);
                    log_ts(stdout);
                    printf(" SCSD-I-PSCLIENT, MSCP$DISK HIT; OUR MSCP$DISK connect"
                           " local=0x%08X seq=%u (disk-discovery step 5)\n",
                           (unsigned)OVMX_PS_MSCP_CONID, ps->psc_mscp_req_seq);
                    fflush(stdout);
                }
                return;
            }
            return; /* any other PS-dir frame (e.g. op=1 echo): already acked */
        }

        /* --- frames on OUR PS MSCP$DISK connection --- */
        if (ovmx_conid_is_class(ps_rconid, OVMX_CONID_CLS_PSMSCP) && ps_lconid != 0) {
            struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
            if (ps == NULL) {
                return;
            }
            /* (A) VAX1's op=2 accept binds OUR MSCP connection -> send op=3
             * confirm, then the FIRST MSCP command (SET CONTROLLER
             * CHARACTERISTICS). af2 sends SCC twice; the 2nd is sent on the
             * first SCC-END below. */
            if (!ps->psc_mscp_connected && ps_dop == SCS_DIR_OP_RESPONSE) {
                ps->psc_mscp_remote_conid = ps_lconid;
                ps->psc_mscp_connected = 1;
                ps_send_mscp_confirm(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                     rx->our_src_logical);
                ps->psc_step = PSC_SCC;
                ps->psc_retx = 0;
                ps_send_scc(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac, rx->our_src_logical);
                log_ts(stdout);
                printf(" SCSD-I-PSCLIENT, OUR MSCP$DISK bound (remote=0x%08X);"
                       " confirm + SET CONTROLLER CHARACTERISTICS (disk-discovery"
                       " step 6)\n", ps_lconid);
                fflush(stdout);
                return;
            }
            /* (B) an MSCP END response (opcode | 0x80). MSCP data frames
             * carry op[46:48]=0x0a (== SCS_DIR_OP_LOOKUP); gating on it keeps
             * a retransmitted op=2 accept from ever being mis-parsed as data. */
            struct scs_mscp_view mvv;
            if (ps_dop == SCS_DIR_OP_LOOKUP &&
                scs_mscp_parse(buf, (size_t)n, &mvv) == 0 && mvv.is_end) {
                if (mvv.opcode == (SCS_MSCP_OP_SET_CTLR_CHAR | SCS_MSCP_END_BIT) &&
                    ps->psc_step == PSC_SCC) {
                    /* af2 sends SCC TWICE before GUS: the 2nd SCC on the first
                     * SCC-END; begin the GUS enumeration on the second. */
                    if (ps->psc_scc_sent < 2) {
                        ps_send_scc(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                    rx->our_src_logical);
                        log_ts(stdout);
                        printf(" SCSD-I-PSCLIENT, SCC-END #%ld (status 0x%04x);"
                               " 2nd SCC sent\n", ps->psc_scc_sent, mvv.status);
                        fflush(stdout);
                    } else {
                        ps->psc_step = PSC_GUS;
                        ps->psc_gus_sent = 0;
                        ps->psc_mscp_msgid = 0; /* reseed the GUS token stream */
                        ps_send_gus(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                    rx->our_src_logical, 0x0001);
                        log_ts(stdout);
                        printf(" SCSD-I-PSCLIENT, SCC complete; GET UNIT STATUS"
                               " enumeration begins (unit 0x0001, step 7)\n");
                        fflush(stdout);
                    }
                } else if (mvv.opcode ==
                               (SCS_MSCP_OP_GET_UNIT_STATUS | SCS_MSCP_END_BIT) &&
                           ps->psc_step == PSC_GUS) {
                    /* vms-533: compare the AA-L619A-TK sec 5.6 MAJOR code, not
                     * the whole status word. Table B-2 publishes Unit-Offline
                     * sub-codes 1/2/4/8 (0x23, 0x43, 0x83, 0x103) -- a
                     * `status == 3` test terminates the walk on none of them. */
                    if (mvv.status_major == SCS_MSCP_ST_OFFLINE) {
                        /* end-of-list terminator: disk discovery COMPLETE. VAX1
                         * self-triggers reconfiguration -> op0x03 COMMIT ->
                         * MEMBER on its next poll (~1.7s); no further client
                         * frame is required (af2 grounded). */
                        ps->psc_step = PSC_DONE;
                        log_ts(stdout);
                        printf(" SCSD-I-PSDONE, MSCP disk discovery complete:"
                               " %ld unit(s) AVAILABLE + OFFLINE terminator."
                               " Awaiting VAX1 reconfiguration -> MEMBER\n",
                               ps->psc_gus_avail);
                        fflush(stdout);
                    } else {
                        if (mvv.status_major == SCS_MSCP_ST_AVAILABLE) {
                            ps->psc_gus_avail++;
                        }
                        /* NEXT-UNIT: next unit = returned unit-word + 1. */
                        uint16_t next_unit = (uint16_t)(mvv.unit + 1);
                        ps_send_gus(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                    rx->our_src_logical, next_unit);
                        log_ts(stdout);
                        printf(" SCSD-I-PSCLIENT, GUS-END unit=0x%04x status=0x%04x;"
                               " next GUS unit=0x%04x (%ld avail)\n",
                               mvv.unit, mvv.status, next_unit, ps->psc_gus_avail);
                        fflush(stdout);
                    }
                }
                return;
            }
            return; /* any other PS-mscp frame (e.g. op=1 echo): already acked */
        }
    }

    /* (b1) vms-d94: the member ACCEPTS OUR active-joiner VMS$VAXcluster
     * CONNECT-REQUEST. GROUNDED on the live wire (d94-fix2.pcap idx34): once
     * OVMX sends its CONNECT-REQUEST promptly after START, the member stops
     * re-issuing START and replies with a frame (observed op 0x5b, the
     * directory/resolution class) whose remote Con.ID == OVMX_JOINER_CONID
     * (our handle, echoed) and whose local Con.ID is the member's own
     * freshly-supplied handle -- i.e. the CONNECT-RESPONSE that binds OUR
     * joiner connection. scs_dir_parse does not classify it (remote != 0, op
     * != 0x0a) and it is not a 0x4b SEQAPP, so catch it here by the Con.ID
     * signature and drive the add-member burst on the bound joiner VC. */
    if (rx->do_connect && n >= 72 &&
        (buf[30] == SCS_DIR_OPCODE || buf[30] == SCS_DIR_OPCODE_RETX ||
         buf[30] == SCS_MSGTYPE_SEQAPP)) {
        uint32_t rconid = (uint32_t)buf[64] | ((uint32_t)buf[65] << 8) |
                          ((uint32_t)buf[66] << 16) | ((uint32_t)buf[67] << 24);
        uint32_t lconid = (uint32_t)buf[68] | ((uint32_t)buf[69] << 8) |
                          ((uint32_t)buf[70] << 16) | ((uint32_t)buf[71] << 24);

        /* vms-578 INTEGRATION: BOTH sides added distinct handling to (b1) and
         * both are kept, closure first. They key on DIFFERENT Con.IDs and
         * cannot both claim a frame: the worktree-760 blocks below are gated
         * on rconid == OVMX_MSCP_CONID, which is a handle the CDL does
         * not hold. The final JOINER_CONID bind takes worktree-760's op
         * discriminator (dop == SCS_DIR_OP_RESPONSE), because the member's
         * lookup-RESPONSE echoes the same rconid and would otherwise be
         * mis-read as a connect-RESPONSE. */

        /* vms-dd5/vms-a61: THE CONNECTION-CONTROL CLASSIFIER USED TO LIVE
         * HERE and no longer does -- it moved to the shared envelope receive
         * block above (scsd_handle_frame's `h.kind == SCS_RX_CONTROL` arm),
         * which already holds this exact frame's parsed envelope and no
         * longer needs a second, independent scs_env_parse_frame() call to
         * get it. See that arm for the full grounding, the reordering-safety
         * argument and the one behaviour delta it names.
         *
         * rx_control_nonconformant keeps its ORIGINAL meaning here rather
         * than moving with the rest: it measures how often this (b1) marker
         * gate (`n>=72 && buf[30] in {0x4b,0x5b,0x7b}`) admits a frame the
         * envelope test refuses (tools/cluster/scs_env_measure.py part (F)),
         * which is a property of THIS gate, not of the now-relocated
         * dispatch -- so it is measured with its own cheap parse rather than
         * inferred from whether the shared block happened to step anything. */
        {
            struct scs_env cenv_diag;
            if (scs_env_parse_frame(buf, (size_t)n, &cenv_diag) != 0) {
                rx_control_nonconformant++;
            }
        }

        /* vms-760: op field [46:48] (abs 60) distinguishes the member's
         * connect-RESPONSE (op=2, binds a connection) from its lookup-RESPONSE
         * (op=0x0a, answers one of OUR dir-client lookups). Both echo OUR
         * handle in rconid, so the sequencer keys on op, not just the pair. */
        uint16_t dop = (n >= 62) ? ((uint16_t)buf[60] | ((uint16_t)buf[61] << 8)) : 0xffff;

        /* vms-760: the member ACCEPTS OUR MSCP$DISK client connect (op=2,
         * remote == our MSCP handle, local = member's fresh MSCP handle;
         * clean-ref SCA idx40). Con.ID-signature, opcode-agnostic (spec sec
         * 4L(3)). On bind, advance the sequencer: resolve VMS$VAXcluster next. */
        /* vms-760: MSCP END response on OUR bound MSCP$DISK connection -- advance
         * the discovery walk. SCC-END (0x84) -> next SCC or first GUS; GUS-END
         * (0x83) -> next unit = returned unit-word + 1, until status OFFLINE ends
         * the list. GROUNDED on vax3-2to3. */
        if (ovmx_conid_is_class(rconid, OVMX_CONID_CLS_MSCP) && dop != SCS_DIR_OP_RESPONSE) {
            struct scs_mscp_view mv2;
            if (scs_mscp_parse(buf, (size_t)n, &mv2) == 0 && mv2.is_end) {
                struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
                if (ps != NULL && ps->mscp_connected && !ps->mscp_disc_done) {
                    scs_vc_note_recv(&ps->vc, mv2.send_seq);
                    if (mv2.base_opcode == SCS_MSCP_OP_GET_UNIT_STATUS) {
                        /* vms-533: the sec 5.6 MAJOR code. `status & 0xff` was
                         * wrong twice over -- the major code is 5 bits, so any
                         * Table B-2 sub-code (Unit-Offline 1/2/4/8 = 0x23/0x43/
                         * 0x83/0x103) escaped the terminator test. */
                        if (mv2.status_major == SCS_MSCP_ST_OFFLINE) {
                            ps->mscp_disc_done = 1;
                            log_ts(stdout);
                            printf(" SCSD-I-MSCPDISC, disk discovery COMPLETE"
                                   " (OFFLINE terminator after %u unit(s))\n",
                                   (unsigned)ps->mscp_msg_id);
                            fflush(stdout);
                        } else {
                            ps->mscp_next_unit = (uint16_t)(mv2.unit + 1);
                        }
                    }
                    if (!ps->mscp_disc_done) {
                        ps_mscp_disc(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                     rx->our_src_logical);
                    }
                }
                return;
            }
        }
        /* vms-e81: FORM B -- the peer's answer to OUR MSCP$DISK CONNECT_REQ
         * carries op [46:48]=4 (once named ACCEPT4 by the vms-760 misreading)
         * instead of an op-2 RESPONSE, and used to be answered with an op-5
         * (once named CONFIRM5) instead of an op-3 CONFIRM.
         *
         * vms-754 (2026-08-06) DECODED [46:48]=4/5 as the shared-namespace
         * REJECT_REQ/REJECT_RSP, not an MSCP connect-ACCEPT/CONFIRM pair --
         * "nothing follows it" (the Con.ID pair never carries application
         * traffic again) is precisely the terminal-dialogue signature
         * tools/cluster/scs_t45_measure.py found on EVERY MTYPE-4/5 exchange
         * in the 47-capture lab-1 library (733/733 terminal, 0 ever followed
         * by application traffic) against 388/394 for the undisputed
         * ACCEPT_REQ (MTYPE 2) positive control -- see
         * docs/cluster-protocol-spec.md sec 4h(1h) and scs_dir.h's
         * SCS_DIR_OP_ACCEPT entry. vms-754 fixed the DECODE only and left the
         * WIRE BEHAVIOUR as a named follow-up: this branch, when it fired
         * against a REAL peer, marks ps->mscp_connected = 1 on a connection
         * the peer actually REJECTED -- a false positive that stalled disk
         * discovery forever believing a refused connect had succeeded.
         *
         * vms-257 (2026-08-08) FIXES the wire behaviour: an op-4 REJECT_REQ
         * answering OUR MSCP$DISK connect no longer binds it. OVMX still
         * sends the op-5 reply the wire pairing requires (62-byte REJECT_REQ
         * -> 58-byte REJECT_RSP, 696 pairs over 26 pcaps,
         * docs/cluster-protocol-spec.md sec 4h(1b) -- the byte shape
         * scs_dir_build_mscp_confirm5() emits was always correct; only what
         * the caller believed it MEANT was wrong), but leaves
         * ps->mscp_connected at 0 and ps->join_step at JS_MSCP_CONNECT, so
         * the existing JOIN_RETX_MAX retransmit timer (below, case
         * JS_MSCP_CONNECT) resends the CONNECT_REQ on schedule -- the same
         * retry-until-accepted pattern the reference itself uses (nine
         * REJECT_REQ/RSP exchanges before a tenth attempt switches to
         * ACCEPT_REQ/ACCEPT_RSP and succeeds, same section). Whether this
         * false-accept explained part of the vms-abd refusal remains a
         * separate, still-open question -- vms-257 fixes the misread, not
         * vms-abd. */
        if (ovmx_conid_is_class(rconid, OVMX_CONID_CLS_MSCP) && lconid != 0 &&
            dop == SCS_DIR_OP_ACCEPT) {
            struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
            if (ps != NULL && !ps->mscp_connected) {
                /* vms-257: do NOT set ps->mscp_remote_conid / mscp_connected
                 * here -- op-4 is the peer REJECTING this connect, not
                 * admitting it. recv accounting is done by the credit block
                 * earlier in the receive path, exactly as for the op-2
                 * sibling below. */
                struct scs_dir_params c5;
                memset(&c5, 0, sizeof(c5));
                memcpy(c5.dst_mac, ps_port_addr(ps), 6);
                memcpy(c5.src_mac, rx->our_hw_mac, 6);
                memcpy(c5.src_logical, rx->our_src_logical, 6);
                memcpy(c5.peer_logical, ps_sys_addr(ps), 6);
                c5.remote_conid = lconid;              /* peer's handle, its op-4 [54] */
                c5.local_conid = PS_MSCP_CONID(ps);    /* ours, from our op-0 */
                c5.incarnation = ps->incarnation;
                c5.recv_ack = ps->vc.seq.recv_seq;
                c5.send_seq = scs_seq_advance(&ps->vc.seq);
                uint8_t f5[SCS_DIR_CONFIRM5_FRAME_LEN];
                if (scs_dir_build_mscp_confirm5(&c5, f5) == 0 &&
                    send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                                  "SCS$DIRECTORY op=5 REJECT_RSP",
                                  f5,
                                  sizeof(f5)) > 0) {
                    log_ts(stdout);
                    printf(" SCSD-W-MSCPREJECTED, peer REJECTED OUR MSCP$DISK"
                           " connect (op-4 REJECT_REQ) -> sent op-5"
                           " REJECT_RSP; NOT binding (local=0x%08X"
                           " remote=0x%08X send_seq=%u). join_step stays"
                           " JS_MSCP_CONNECT so the retransmit timer resends"
                           " CONNECT_REQ\n",
                           (unsigned)PS_MSCP_CONID(ps), lconid, c5.send_seq);
                    fflush(stdout);
                }
            }
            return;
        }
        if (ovmx_conid_is_class(rconid, OVMX_CONID_CLS_MSCP) && lconid != 0 && dop == SCS_DIR_OP_RESPONSE) {
            struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
            if (ps != NULL && !ps->mscp_connected) {
                ps->mscp_remote_conid = lconid;
                ps->mscp_connected = 1;
                log_ts(stdout);
                printf(" SCSD-I-MSCPBOUND, member accepted OUR MSCP$DISK"
                       " connect: local=0x%08X remote=0x%08X\n",
                       (unsigned)PS_MSCP_CONID(ps), lconid);
                fflush(stdout);
                /* vms-760: CONFIRM the MSCP connection (op=3) before proceeding.
                 * GROUNDED on the 2->3 reference: VAX3 sends its MSCP op=3 confirm
                 * (ss=8) BEFORE its VC connect (ss=10). Skipping it leaves the MSCP
                 * connection half-open and the member then only ECHOES the next
                 * connect (op=1) and never op2-accepts it -- the step-7 stall. */
                {
                    struct scs_dir_params mc;
                    memset(&mc, 0, sizeof(mc));
                    memcpy(mc.dst_mac, ps_port_addr(ps), 6);
                    memcpy(mc.src_mac, rx->our_hw_mac, 6);
                    memcpy(mc.src_logical, rx->our_src_logical, 6);
                    memcpy(mc.peer_logical, ps_sys_addr(ps), 6);
                    mc.remote_conid = ps->mscp_remote_conid;
                    mc.local_conid = PS_MSCP_CONID(ps);
                    mc.recv_ack = ps->vc.seq.recv_seq;
                    mc.send_seq = scs_seq_advance(&ps->vc.seq);
                    mc.incarnation = ps->incarnation;
                    uint8_t mf[SCS_DIR_CONFIRM_FRAME_LEN];
                    if (scs_dir_build_connect_confirm(&mc, mf) == 0) {
                        send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                                  "MSCP$DISK op=1 CONNECT-ECHO",
                                  mf, sizeof(mf));
                        scs_vc_record_sent(&ps->vc, mc.send_seq, monotonic_ms());
                    }
                }
                /* vms-760: begin MSCP disk DISCOVERY on the freshly-bound MSCP
                 * connection (SCC x2 -> GUS walk). VAX1 answers the add-member
                 * config only after this completes (vax3-2to3 reference). */
                if (ps_mscp_disc(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac, rx->our_src_logical)) {
                    log_ts(stdout);
                    printf(" SCSD-I-MSCPDISC, started disk discovery (SCC #1)"
                           " on MSCP conid 0x%08X\n", (unsigned)PS_MSCP_CONID(ps));
                    fflush(stdout);
                }
                if (ps->join_step == JS_MSCP_CONNECT) {
                    ps->join_step = JS_DIR_LOOKUP_VC;
                    ps->js_retx = 0;
                    send_own_dir_lookup(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                        rx->our_src_logical, "VMS$VAXcluster", 0);
                    log_ts(stdout);
                    printf(" SCSD-I-JOINSEQ, lookup VMS$VAXcluster seq=%u (step 6/8)\n",
                           ps->js_lookup_seq);
                    fflush(stdout);
                }
            }
            return;
        }
        if (ovmx_conid_is_class(rconid, OVMX_CONID_CLS_JOINER) && lconid != 0 && dop == SCS_DIR_OP_RESPONSE) {
            struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
            if (ps != NULL && !ps->joiner_connected) {
                ps->joiner_remote_conid = lconid;
                ps->joiner_connected = 1;
                /* p. 2-50: "Once SYSAP_A has established a connection with
                 * SYSAP_X, it will disable polling for SYSAP_X on NODE_X."
                 * Polling for VMS$VAXcluster continues on every OTHER node. */
                scs_poll_connected(scsd_poll(rx), "VMS$VAXcluster", ps_sys_addr(ps));
                /* vms-dd5: the member's answer to OUR CONNECT_REQ, carrying
                 * BOTH Con.IDs -- message type 2, ACCEPT_REQ (spec sec
                 * 4(h)(1a)) = op=2 CONNECT-RESPONSE (§4(m)). If the peer's
                 * message-type-1 CONNECT_RSP was seen, the classifier above
                 * already moved this connection to CONNECT ACK and the step
                 * below is the DOCUMENTED Figure 2-14 transition to OPEN; if
                 * that frame was lost, this arrives in CONNECT SENT and runs
                 * through the LABELED OVMX row instead, and its log line says
                 * which. The transition's action -- the FSM names it "send
                 * ACCEPT_RSP" -- IS the op=3 CONNECT-CONFIRM that OVMX builds
                 * and sends just below (§4(O.8)); it is passed to conn_step so
                 * the machine records the emit instead of falsely reporting it
                 * unemitted. */
                scs_cdt_set_remote_conid(ps->cdt_joiner, lconid);
                log_ts(stdout);
                printf(" SCSD-I-JOINBOUND, member accepted OUR VMS$VAXcluster"
                       " connect: local=0x%08X remote=0x%08X\n",
                       PS_JOINER_CONID(ps), lconid);
                fflush(stdout);
                /* vms-760/§4(m): CONFIRM the VMS$VAXcluster VC (op=3) BEFORE the
                 * add-member burst -- the same load-bearing confirm the MSCP
                 * connection gets above.
                 * GROUNDED, vax3-2to3-established-join-20260730.pcap: VAX3's VC
                 * CONNECT-REQ is frame 132 (ss=10), VAX1's op=2 ACCEPT frame 136
                 * (ss=11, +0.9391), VAX3's op=3 CONFIRM frame 139 (ss=13, +0.9393,
                 * 76 bytes, mt=0x5b, recv_ack=11) and ONLY THEN the two 204-byte
                 * config frames 142/143 (ss=14,15, +0.9394). VAX1 answers 0.3 ms
                 * later (frames 145/146, +0.9397) and opens its own connections
                 * back at +0.9543.
                 * Without this confirm the VC stays half-open: the member binds
                 * it but its connection manager never runs the add-member dialogue
                 * on it, so the config burst is ignored -- SHOW CLUSTER stays NEW
                 * with ZERO member-initiated traffic back (d94-disc2/disc3.pcap,
                 * where OVMX sends only op=10 on conid 0x4f580002, never op=3).
                 * §4(O.8): this op=3 confirm IS Figure 2-14's ACCEPT_RSP for the
                 * joiner VC. Build and send it FIRST, then hand its identity to
                 * the conn_step below so the FSM's "send ACCEPT_RSP" action is
                 * recorded as emitted -- NOT reported as CONNNOACT/"no builder"
                 * for a frame OVMX has a builder for and puts on the wire twice
                 * (once per peer), measured byte-matching VAX3's frame 139. */
                const char *vc_confirm_emitted = NULL;
                {
                    struct scs_dir_params vc;
                    memset(&vc, 0, sizeof(vc));
                    memcpy(vc.dst_mac, ps_port_addr(ps), 6);
                    memcpy(vc.src_mac, rx->our_hw_mac, 6);
                    memcpy(vc.src_logical, rx->our_src_logical, 6);
                    memcpy(vc.peer_logical, ps_sys_addr(ps), 6);
                    vc.remote_conid = ps->joiner_remote_conid;
                    vc.local_conid = PS_JOINER_CONID(ps);
                    vc.recv_ack = ps->vc.seq.recv_seq;
                    vc.send_seq = scs_seq_advance(&ps->vc.seq);
                    vc.incarnation = ps->incarnation;
                    uint8_t vf[SCS_DIR_CONFIRM_FRAME_LEN];
                    if (scs_dir_build_connect_confirm(&vc, vf) == 0) {
                        send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                                  "VMS$VAXcluster op=3 CONNECT-CONFIRM",
                                  vf, sizeof(vf));
                        scs_vc_record_sent(&ps->vc, vc.send_seq, monotonic_ms());
                        vc_confirm_emitted = "VMS$VAXcluster op=3 CONNECT-CONFIRM";
                        log_ts(stdout);
                        printf(" SCSD-I-JOINCONFIRM, confirmed OUR VMS$VAXcluster"
                               " VC (op=3 seq=%u) before the add-member burst\n",
                               vc.send_seq);
                        fflush(stdout);
                    }
                }
                /* Now record the Figure 2-14 RCV_ACCEPT_REQ->OPEN transition,
                 * telling the machine the op=3 confirm it just emitted. If the
                 * builder above failed, vc_confirm_emitted stays NULL and the
                 * CONNNOACT warning correctly fires (a genuine gap); on the
                 * normal path it is non-NULL and the false alarm is gone. */
                conn_step(ps->cdt_joiner, SCS_CONN_EV_RCV_ACCEPT_REQ,
                          vc_confirm_emitted);
                /* vms-694 (§4(O.7)): the conn_step above is the transition that
                 * reaches OPEN (Figure 2-14) for the VMS$VAXcluster SYSAP
                 * connection the peer holds open. Latch that into OVMX's own
                 * membership self-view now, off the CDT the daemon just drove --
                 * so OVMX models the connection and the fact survives the
                 * graceful teardown that closes the CDT at shutdown. */
                ps_note_vaxcluster_open(ps);
                if (!ps->joiner_cm_sent) {
                    int c = cm_send_config_burst(rx->sock, rx->ifindex, ps, rx->our_hw_mac,
                                                 rx->our_src_logical,
                                                 PS_JOINER_CONID(ps), lconid);
                    rx->cm_config_frames += c;
                    ps->joiner_cm_sent = 1;
                    ps->joiner_cm_ms = monotonic_ms();
                    ps->join_step = JS_ADD_MEMBER; /* await the member's reciprocal config */
                    log_ts(stdout);
                    printf(" SCSD-I-CMCONFIG, sent add-member config burst"
                           " (op 0x14/0x01/0x02, %d frames, VOTES=0 non-voting)"
                           " on OUR joiner VC (step 8/8)\n", c);
                    fflush(stdout);
                }
            }
            return;
        }
        /* vms-760: frames on OUR SCS$DIRECTORY client connection (rconid ==
         * our dir handle). Two kinds, distinguished by op: */
        if (ovmx_conid_is_class(rconid, OVMX_CONID_CLS_DIRJ) && lconid != 0) {
            struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
            if (ps == NULL) {
                return;
            }
            /* (A) the member's LOOKUP-RESPONSE (op=0x0a) to one of OUR
             * dir-client lookups: advance the stop-and-wait sequencer. HIT vs
             * MISS = result [78:94] (abs 92) != "NOT PRESENT HERE" (marker is 1
             * for both -- do NOT key on it). */
            if (dop == SCS_DIR_OP_LOOKUP && n >= 108) {
                int affirmative =
                    memcmp(buf + 92, SCS_DIR_NOT_PRESENT, SCS_DIR_RESULT_LEN) != 0;
                ps->js_retx = 0;
                if (ps->join_step == JS_DIR_LOOKUP_TAPE) {
                    /* MSCP$TAPE expected MISS -> resolve MSCP$DISK next. */
                    ps->join_step = JS_DIR_LOOKUP_DISK;
                    send_own_dir_lookup(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                        rx->our_src_logical, "MSCP$DISK", 0);
                    log_ts(stdout);
                    printf(" SCSD-I-JOINSEQ, MSCP$TAPE %s; lookup MSCP$DISK seq=%u"
                           " (step 4/8)\n", affirmative ? "HIT" : "miss",
                           ps->js_lookup_seq);
                    fflush(stdout);
                } else if (ps->join_step == JS_DIR_LOOKUP_DISK && affirmative) {
                    /* vms-760: the clean joiner sends a SECOND MSCP$DISK
                     * lookup and THEN the connect, pipelined (clean-ref
                     * sca34 seq5 lookup -> sca35 seq6 connect, back-to-back
                     * without waiting for the 2nd response). The live member
                     * accepts the MSCP$DISK connect only after TWO DISK lookup
                     * exchanges -- with ONE, its recv_ack freezes and the
                     * connect is never bound (observed d94-760seq.pcap). Send
                     * lookup #2 (fire-and-forget), then the connect. */
                    send_own_dir_lookup(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                        rx->our_src_logical, "MSCP$DISK", 0);
                    ps->join_step = JS_MSCP_CONNECT;
                    send_mscp_connect_request(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                              rx->our_src_logical);
                    clock_gettime(CLOCK_MONOTONIC, &ps->js_last_tx);
                    log_ts(stdout);
                    printf(" SCSD-I-JOINSEQ, MSCP$DISK HIT; 2nd lookup + MSCP$DISK"
                           " connect local=0x%08X seq=%u (step 5/8)\n",
                           (unsigned)PS_MSCP_CONID(ps), ps->mscp_req_seq);
                    fflush(stdout);
                } else if (ps->join_step == JS_DIR_LOOKUP_VC && affirmative &&
                           cm_rejoin_target_mode()) {
                    /* vms-760 / vms-4838: run the joiner's CLIENT half (own
                     * directory + MSCP$DISK discovery) but do NOT open our own
                     * VMS$VAXcluster VC -- leave that to the member, and let
                     * the add-member dialogue ride the VC IT opens.
                     *
                     * vms-4838: this branch now fires AUTOMATICALLY on a REJOIN
                     * (cm_rejoin_target_mode() -> ovmx_prior.valid), not only
                     * under the manual OVMX_NO_OWN_VC override. GROUNDED (spec
                     * 4(O.11)): a crash-rejoiner never opens its own outbound
                     * VMS$VAXcluster connect; the members open one *to* it and
                     * it drives CM readmission as the TARGET. Opening our own
                     * outbound here is exactly the arm-C/arm-D bug -- the member
                     * refuses/ignores it and OVMX's op 0x02 never reaches the
                     * connection the member is actually listening on.
                     *
                     * GROUNDED motivation, vax3-2to3-established-join pcap:
                     * there is exactly ONE VC per node pair, and the peer that
                     * actually DROVE the commit was VAX2 -- over a VC VAX2
                     * itself opened (frame 208). VAX3 had opened its own VC to
                     * VAX1 (frame 132), and VAX1 never drove a commit on it;
                     * VAX3 sent VAX1 its op 0x02 only at +13.6761, AFTER it was
                     * already MEMBER. Because OVMX opens its own VC to EVERY
                     * member, no member ever opens one back, so nothing ever
                     * plays VAX2's role. */
                    ps->join_step = JS_ADD_MEMBER;
                    ps->js_retx = 0;
                    log_ts(stdout);
                    printf(" SCSD-I-JOINSEQ, VMS$VAXcluster HIT; NOT opening our own"
                           " VC (%s) -- awaiting the member's VC connect,"
                           " then driving CM readmission on it as the Figure-2-14"
                           " TARGET (spec 4(O.11))\n",
                           getenv("OVMX_NO_OWN_VC") != NULL
                               ? "OVMX_NO_OWN_VC"
                               : "REJOIN, member-initiated topology");
                    fflush(stdout);
                } else if (ps->join_step == JS_DIR_LOOKUP_VC && affirmative &&
                           ps->appeared_after_join && ps->connected) {
                    /* vms-e81: a NEWCOMER we are meeting as a MEMBER. Our
                     * client half stops here. We must NOT open a
                     * VMS$VAXcluster VC to it and must NOT run admission at
                     * it: grounded from VAX1's side of vax3-2to3, an existing
                     * member never opened a VC to the joiner -- the JOINER had
                     * already opened one, exactly as spec 4(o) says (a member
                     * opens its own VC only if the joiner has not). Admission
                     * is the newcomer's business with the coordinator; our job
                     * is to be reachable and to answer the barrier. */
                    /* The newcomer has ALREADY opened a VC to us (ps->connected),
                     * so we must not open a second one -- spec 4(o): a member
                     * opens its own VC only if the joiner has not. Our client
                     * half is done; we are reachable, and admission is the
                     * newcomer's business with the coordinator. */
                    ps->join_step = JS_IDLE;
                    ps->js_retx = 0;
                    log_ts(stdout);
                    printf(" SCSD-I-MEMBGREET, newcomer client-half complete"
                           " (dir + MSCP$DISK); it already opened a VC to us"
                           " so we open none -- awaiting the coordinator's"
                           " barrier\n");
                    fflush(stdout);
                } else if (ps->join_step == JS_DIR_LOOKUP_VC && affirmative) {
                    /* VMS$VAXcluster resolved -> open the VC connect. */
                    ps->join_step = JS_VC_CONNECT;
                    /* vms-578: the vms-398 signature -- name the NODE, not the
                     * circuit; CONNECT selects the OPEN VC via CONFIG_SYS
                     * (p. 2-47). */
                    send_joiner_connect_request(rx->sock, rx->ifindex, rx->cfg, ps, NULL,
                                                rx->our_hw_mac, rx->our_src_logical);
                    clock_gettime(CLOCK_MONOTONIC, &ps->js_last_tx);
                    rx->connect_req_sent++;
                    log_ts(stdout);
                    printf(" SCSD-I-JOINSEQ, VMS$VAXcluster HIT; VC connect"
                           " local=0x%08X seq=%u (step 7/8)\n",
                           PS_JOINER_CONID(ps), ps->joiner_req_seq);
                    fflush(stdout);
                }
                return;
            }
            /* (B) the member's CONNECT-RESPONSE (op=2) binding OUR dir
             * connection: send the op=3 confirm and START the SYSAP lookups
             * (MSCP$TAPE first) -- the choreography the member requires before
             * it will accept the MSCP$DISK / VC connects (NEW->MEMBER). */
            if (!ps->own_dir_connected && dop == SCS_DIR_OP_RESPONSE) {
                ps->own_dir_remote_conid = lconid;
                ps->own_dir_connected = 1;
                log_ts(stdout);
                printf(" SCSD-I-OWNDIRBOUND, member accepted OUR SCS$DIRECTORY"
                       " connect: local=0x%08X remote=0x%08X\n",
                       (unsigned)PS_SCS_DIR_JOINER_CONID(ps), lconid);
                fflush(stdout);
                /* op=3 confirm (fire-and-forget), then the first lookup. */
                send_own_dir_confirm(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                     rx->our_src_logical);
                ps->join_step = JS_DIR_LOOKUP_TAPE;
                ps->js_retx = 0;
                send_own_dir_lookup(rx->sock, (int)rx->ifindex, ps, rx->our_hw_mac,
                                    rx->our_src_logical, "MSCP$TAPE", 0);
                log_ts(stdout);
                printf(" SCSD-I-JOINSEQ, dir confirm + lookup MSCP$TAPE seq=%u"
                       " (steps 2-3/8)\n", ps->js_lookup_seq);
                fflush(stdout);
            }
            return;
        }
    }

    /* (b1.5) vms-760 SERVER-FIRST established-join: OVMX serves the
     * MEMBER-OPENED MSCP$DISK connect. When an established member admits a
     * first-timer joiner it OPENS an MSCP$DISK SCS connection TO the joiner
     * (member = VMS$DISK_CL_DRVR client) and expects the joiner (OVMX, the
     * MSCP$DISK server) to ACCEPT it. Reference af2-firsttimer-established
     * pcap (cycle 1, rel~143.758): M->J op=0 connect (msgtype 0x4b, name@76
     * 'MSCP$DISK', remote Con.ID 0, local Con.ID = member's MSCP CLIENT
     * handle) -> OVMX replies op=1 echo then op=4 accept (binding a FRESH
     * OVMX MSCP server handle) -> member sends op=5 confirm. This is additive
     * server behavior and runs on the DEFAULT path (independent of the
     * OVMX_JOIN_SEQ sequencer). Placed BEFORE the (c) SCS-envelope handler so
     * the MSCP$DISK connect is not mis-answered as a VMS$VAXcluster connect
     * (both carry remote Con.ID 0; the name@76 is the discriminator). */
    if (rx->do_connect && n >= 72 &&
        (buf[30] == SCS_MSGTYPE_SEQAPP || buf[30] == SCS_DIR_OPCODE ||
         buf[30] == SCS_DIR_OPCODE_RETX)) {
        uint16_t mop = (uint16_t)buf[60] | ((uint16_t)buf[61] << 8);
        uint32_t m_rconid = (uint32_t)buf[64] | ((uint32_t)buf[65] << 8) |
                            ((uint32_t)buf[66] << 16) | ((uint32_t)buf[67] << 24);
        uint32_t m_lconid = (uint32_t)buf[68] | ((uint32_t)buf[69] << 8) |
                            ((uint32_t)buf[70] << 16) | ((uint32_t)buf[71] << 24);
        uint16_t m_seq = (uint16_t)buf[34] | ((uint16_t)buf[35] << 8);

        /* op=0 CONNECT-REQUEST naming MSCP$DISK (remote handle not yet
         * learned). n >= 92 so name@76 [76:92] is in-frame. */
        if (ovmx_mscp_server_enabled() &&
            mop == SCS_DIR_OP_CONNECT && m_rconid == 0 && n >= 92 &&
            memcmp(buf + 76, "MSCP$DISK       ", 16) == 0) {
            /* vms-578: gated with the LISTEN registration and the lookup
             * answer, by one flag, so OVMX cannot accept a connection for a
             * SYSAP its own directory says it does not serve. With the flag OFF
             * this request falls through to the p. 2-48 scan below and is
             * REFUSED, which is work/vms-187-closure's behaviour unchanged. */
            struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
            if (ps != NULL) {
                if (!ps->vc.initialized) {
                    scs_vc_init(&ps->vc);
                }
                ps_learn_sys_addr(rx->cfg, ps, buf + OFF_HELLO_SRCLOG); /* member src-logical */
                scs_vc_note_recv(&ps->vc, m_seq);
                /* vms-298: capture the PREVIOUS handle before overwriting it --
                 * the retransmit test below compares against it, and assigning
                 * first would make that test trivially true. */
                uint32_t prev_srv_remote_conid = ps->mscp_srv_remote_conid;
                ps->mscp_srv_remote_conid = m_lconid; /* member's MSCP CLIENT handle */

                /* Idempotent stop-and-wait: allocate the echo/accept send_seq
                 * ONCE on first connect and REUSE it on every retransmit of the
                 * member's op=0 (never re-advance -- the shared per-channel
                 * send_seq must stay contiguous; re-advancing on each retransmit
                 * poisons the sequence exactly like the 760mscp hole). Mirrors
                 * the "allocate once" pattern of joiner_req_seq/mscp_req_seq. */
                /* vms-298: A RETRANSMIT AND A SECOND CONNECTION ARE NOT THE
                 * SAME EVENT, AND THE Con.ID IS WHAT TELLS THEM APART.
                 *
                 * This gate used to read `ps->mscp_srv_bound` -- "have we ever
                 * bound one of these?" -- so a genuinely NEW MSCP$DISK connect
                 * from the same peer was treated as a retransmit of the first
                 * and answered by replaying the send_seqs allocated minutes
                 * earlier. GROUNDED in ovmx-760-MEMBER-achieved: ~10 s after
                 * the first bind each VAX opens a SECOND MSCP$DISK connect
                 * (frames 2890/2896/2920); OVMX answered with send_seq 27/28
                 * while its live per-peer counters stood at 40/180/320. All
                 * three were SILENTLY DROPPED -- no op-5 confirm ever arrived
                 * and not one further frame flowed on those Con.IDs. The
                 * identical op1/op4 pair at frame 188, same bytes, was
                 * confirmed at 190; the only difference was a contiguous seq.
                 *
                 * A true retransmit carries the SAME peer Con.ID; a new
                 * connection carries a different one. So key on that.
                 *
                 * PARTLY FIXED by vms-694, the rest still needs its own
                 * grounding. The CROSS-PEER half is fixed: OVMX now offers a
                 * PER-PEER server handle (PS_MSCP_SERVER_CONID(ps)), so two
                 * DIFFERENT members no longer collide on one CDL slot. What is
                 * still node-fixed is the SAME peer opening a SECOND MSCP$DISK
                 * connection: it is offered the SAME per-peer handle as the
                 * first (one server slot per peer), where a real node allocates
                 * a fresh pair per connection. That residual anomaly and the
                 * seq one below were present on those three frames at once and
                 * the capture cannot separate them, so vms-298 fixed the seq
                 * (unambiguously wrong) and left the per-connection handle
                 * visible. */
                int retx = ps->mscp_srv_bound &&
                           prev_srv_remote_conid == m_lconid;
                if (!retx) {
                    if (ps->mscp_srv_bound) {
                        log_ts(stdout);
                        printf(" SCSD-I-MSCPSRV, peer opened a SECOND MSCP$DISK"
                               " connect (remote 0x%08X -> 0x%08X) -- allocating"
                               " FRESH send_seqs, not replaying the first bind's\n",
                               prev_srv_remote_conid, m_lconid);
                        fflush(stdout);
                    }
                    ps->mscp_srv_echo_seq   = scs_seq_advance(&ps->vc.seq);
                    ps->mscp_srv_accept_seq = scs_seq_advance(&ps->vc.seq);
                }

                struct scs_dir_params dp;
                memset(&dp, 0, sizeof(dp));
                memcpy(dp.dst_mac, ps_port_addr(ps), 6);
                memcpy(dp.src_mac, rx->our_hw_mac, 6);
                memcpy(dp.src_logical, rx->our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
                memcpy(dp.peer_logical, ps_sys_addr(ps), 6);
                dp.remote_conid = m_lconid; /* echo the member's client handle */
                dp.incarnation = ps->incarnation; /* §4i established-join echo */

                /* op=1 echo (local Con.ID stays 0). */
                dp.local_conid = 0;
                dp.recv_ack = ps->vc.seq.recv_seq;
                dp.send_seq = ps->mscp_srv_echo_seq;
                /* vms-760: answer in the CONNECTION's phase (see the
                 * SCS$DIRECTORY accept path). An established member opens this
                 * MSCP$DISK connect with the data-phase msgtype; replying with
                 * the builder's establishing-phase default made our ACCEPT4
                 * arrive as 0x5b against a 0x4b request (d94-e6 frame 595). */
                uint8_t mscp_mt = buf[30];
                uint8_t eframe[SCS_DIR_ECHO_FRAME_LEN];
                if (scs_dir_build_mscp_echo(&dp, eframe) == 0) {
                    eframe[30] = mscp_mt;
                    send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                                  "op=1 CONNECT-ECHO",
                                  eframe, sizeof(eframe));
                }

                /* vms-257: op=2 ACCEPT_REQ (CONNECT-RESPONSE) binding OUR fresh
                 * MSCP server handle -- Figure 2-14's target-column accept. This
                 * REPLACES the op=4 scs_dir_build_mscp_accept the server-first
                 * path used to emit: op 4 is the shared-namespace REJECT_REQ
                 * (vms-754), so a real class driver read OVMX's "accept" as a
                 * REJECT, abandoned the connection and retried every ~10s with a
                 * fresh Con.ID, never reaching OPEN and never sending an MSCP
                 * command (vms-257, live lab-2 vaxlab-0 2026-08-28). A genuine
                 * accept is op 2 (ACCEPT_REQ), which the client-side receive path
                 * already reads as "member accepted OUR connect". */
                dp.local_conid = PS_MSCP_SERVER_CONID(ps);
                dp.recv_ack = ps->vc.seq.recv_seq;
                dp.send_seq = ps->mscp_srv_accept_seq;
                uint8_t aframe[SCS_DIR_CONFIRM_FRAME_LEN];
                if (scs_dir_build_mscp_response(&dp, aframe) == 0 &&
                    (aframe[30] = mscp_mt, 1) &&
                    send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                                  "op=2 CONNECT-RESPONSE (ACCEPT_REQ)",
                                  aframe,
                                  sizeof(aframe)) > 0) {
                    ps->mscp_srv_bound = 1;
                    ps->mscp_srv_accepts++;
                    rx->mscp_srv_accepts++;
                    log_ts(stdout);
                    printf(" SCSD-I-MSCPSRV, accepted member MSCP$DISK connect:"
                           " local=0x%08X remote=0x%08X (server-first)\n",
                           (unsigned)PS_MSCP_SERVER_CONID(ps), m_lconid);
                    fflush(stdout);

                    /* vms-34b: GIVE THE CONNECTION A CDT, so the commands the
                     * class driver is about to send have somewhere to be
                     * delivered. Before this item scsd.c never allocated one
                     * at the MSCP server handle (scs_mscp.h's own comment
                     * names this), so scs_cdl_deliver_message() always
                     * resolved SCS_DELIVER_NO_CDT here and every command was
                     * dropped in silence -- accept-then-black-hole, the exact
                     * facade this posture exists to avoid. vms-694: the server
                     * handle is now PER-PEER (PS_MSCP_SERVER_CONID(ps)), so two
                     * DIFFERENT members get distinct CDTs; only the SAME peer's
                     * SECOND MSCP$DISK connect finds its own SAME CDT already
                     * allocated (the residual per-connection limit above), in
                     * which case we re-point its remote_conid at the new
                     * m_lconid rather than allocating the same slot twice. */
                    struct scs_cdt *mcdt = scs_cdl_lookup(&scsd_cdl,
                                                          PS_MSCP_SERVER_CONID(ps));
                    if (mcdt == NULL) {
                        mcdt = scs_cdl_alloc_conid(&scsd_cdl,
                                                   PS_MSCP_SERVER_CONID(ps),
                                                   "MSCP$DISK",
                                                   "VMS$DISK_CL_DRVR", ps->pb);
                    }
                    if (mcdt != NULL) {
                        scs_cdt_set_remote_conid(mcdt, m_lconid);
                        scs_cdt_set_handlers(mcdt, scsd_mscp_srv_msg_input,
                                             NULL, NULL, ps);
                        /* vms-257: DRIVE THE SERVER CONNECTION FSM so its state
                         * reflects the accept we just sent. Figure 2-14's target
                         * column: the inbound CONNECT_REQ took the connection to
                         * CONNECT REC (we sent the op=1 CONNECT_RSP echo above),
                         * and invoking ACCEPT (our op=2 ACCEPT_REQ) takes it to
                         * ACCEPT SENT, awaiting the client's op=3 ACCEPT_RSP that
                         * opens it. Both frames were already emitted here, so the
                         * FSM actions are passed a non-NULL `emitted` and never
                         * log an unemitted-action warning. On a retransmit the
                         * connection may already be past CLOSED; the RCV_CONNECT_REQ
                         * tolerance rows keep that legal, and SVC_ACCEPT is only
                         * re-driven from CONNECT REC. */
                        conn_step(mcdt, SCS_CONN_EV_RCV_CONNECT_REQ,
                                  "op=1 CONNECT_RSP echo");
                        if (scs_conn_state_of(mcdt) == SCS_CONN_CONNECT_REC) {
                            conn_step(mcdt, SCS_CONN_EV_SVC_ACCEPT,
                                      "op=2 ACCEPT_REQ");
                        }
                    } else {
                        log_ts(stdout);
                        printf(" SCSD-W-MSCPSRVNOCDT, accepted the connect but"
                               " could not allocate its CDT -- commands on"
                               " this connection will be dropped, not"
                               " answered\n");
                        fflush(stdout);
                    }
                    /* vms-94c: ready this node's DLM server CDT for the same
                     * peer, so a cross-node DLM message that arrives on our DLM
                     * server handle is decoded + dispatched (rung 1) rather than
                     * black-holed. Additive: PS_DLM_SERVER_CONID is a fresh
                     * per-peer Con.ID class no other path uses. Rung 2 adds the
                     * connect choreography that teaches the peer this handle. */
                    (void)scsd_dlm_ensure_server_cdt(ps);
                }
            }
            return;
        }

        /* vms-257: THERE IS NO op=5 "CONFIRM" HANDLER HERE ANY MORE.
         *
         * It used to set ps->mscp_srv_confirmed = 1 and log SCSD-I-MSCPSRVOK on
         * an inbound op=5, believing op 5 was the member's confirm of OUR server
         * connection. vms-754 decoded op 5 as the shared-namespace REJECT_RSP,
         * so that handler read a REJECTION as a confirmation and logged success
         * while the connection FSM was still CLOSED and the peer disagreed --
         * exactly the fabricated-success shape INV-6 forbids (proven live on
         * lab-2 vaxlab-0 2026-08-28: the FSM logged the op-5 as an illegal
         * RCV_REJECT_RSP while MSCPSRVOK claimed "confirmed").
         *
         * The honest confirm is the client's op=3 ACCEPT_RSP, which the shared
         * envelope classifier (scsd_handle_frame's SCS_RX_CONTROL arm) steps
         * through the connection FSM; MSCPSRVOK is now logged there, and only
         * when the FSM actually reaches OPEN. An inbound op=5 REJECT_RSP now
         * falls through to that same FSM step, which records it honestly (an
         * illegal event in any server state, since after this fix OVMX never
         * sends the op-4 REJECT_REQ that would provoke one). */
    }

    /* (b2) vms-246: SCS$DIRECTORY connect + SCS$DIR_LOOKUP responder. After
     * START, the ESTABLISHED node opens an SCS$DIRECTORY SCS connection to
     * the joiner (OVMX) and queries OVMX's directory for each SYSAP it wants
     * to reach. OVMX must (1) bind the SCS$DIRECTORY Con.ID pair by answering
     * the CONNECT-REQUEST with a CONNECT-RESPONSE, and (2) answer each lookup
     * (affirm VMS$VAXcluster -- the connection manager OVMX serves --, and
     * "NOT PRESENT HERE" for SYSAPs it does not) so the VAX resolves OVMX and
     * proceeds to send the VMS$VAXcluster 0x4b CONNECT-REQUEST (spec sec 4h;
     * next item vms-c6d). Only sequenced directory frames (0x5b/0x7b, and the
     * 0x4b form the lookups switch to once the connection is up) reach here;
     * the credit-ack block above already 0x48-acked them. */
    if (rx->do_connect &&
        (buf[30] == SCS_DIR_OPCODE || buf[30] == SCS_DIR_OPCODE_RETX ||
         buf[30] == SCS_MSGTYPE_SEQAPP)) {
        struct scs_dir_view dv;

        /* (b2.0) vms-66f: frames addressed to the connection OUR SCS$DIR_LOOKUP
         * opened. These are the ANSWER half of p. 2-50 and are the only frames
         * in this file whose [50:54] names the poller's own handle, so the test
         * is exact and cannot capture the peer-opened directory connection. */
        if (scs_dir_parse(buf, (size_t)n, &dv) == 0 &&
            ovmx_conid_is_class(dv.remote_conid, OVMX_CONID_CLS_DIRPOLL)) {
            struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
            if (ps == NULL) {
                return;
            }
            if (!ps->vc.initialized) {
                scs_vc_init(&ps->vc);
            }
            scs_vc_note_recv(&ps->vc, dv.send_seq);
            if (dv.local_conid != 0) {
                ps->poll_remote_conid = dv.local_conid;
                /* vms-66f r4: AND ON THE DESCRIPTOR. p. 2-29 puts the Con.ID
                 * PAIR on the CDT, and the teardown is addressed from there --
                 * scsd_svc_emit_disconnect() reads cdt->remote_conid, not the
                 * peer slot. Recording the handle only on the peer slot was
                 * invisible while the poller never disconnected; the moment it
                 * did, its DISCONNECT_REQ named remote Con.ID 0, which names no
                 * connection at the peer. Same call the receive path already
                 * makes for the two VMS$VAXcluster connections. */
                struct scs_cdt *pc = scs_cdl_lookup(&scsd_cdl,
                                                    PS_SCS_DIR_POLL_CONID(ps));
                if (pc != NULL) {
                    scs_cdt_set_remote_conid(pc, dv.local_conid);
                }
            }
            if (dv.is_lookup_response) {
                /* p. 2-50's "Yes"/"No". scs_poll_answer() decides who, if
                 * anyone, is notified -- this branch decides nothing. */
                rx->poll_answers++;
                log_ts(stdout);
                printf(" SCSD-I-POLLANS, SCS$DIRECTORY answered '%s' -> %s\n",
                       dv.name, scs_dir_answer_name(dv.answer));
                fflush(stdout);
                (void)scs_poll_answer(scsd_poll(rx), dv.name, dv.answer, monotonic_ms());
            } else if (dv.op == SCS_DIR_MSGTYPE_ACCEPT_REQ) {
                /* "and the Directory Service accepts the connection" (p. 2-50):
                 * the message-type-2 ACCEPT_REQ, GROUNDED in sec 4h(1a). */
                (void)scs_poll_opened(scsd_poll(rx), monotonic_ms());
            }
            return;
        }

        if (scs_dir_parse(buf, (size_t)n, &dv) == 0 &&
            (dv.is_dir_connect_request || dv.is_lookup_request)) {
            struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
            if (ps == NULL) {
                return;
            }
            if (!ps->vc.initialized) {
                scs_vc_init(&ps->vc);
            }
            ps->dir_seen = 1;
            ps_learn_sys_addr(rx->cfg, ps, buf + OFF_HELLO_SRCLOG); /* src-logical, abs 24 */
            /* Ensure recv_ack is current even if the credit block did not run
             * (e.g. a 0x7b retransmit); note_recv only advances the high-water. */
            scs_vc_note_recv(&ps->vc, dv.send_seq);

            /* vms-abc: the up-front refusal is kept EVEN THOUGH both sends
             * below now go through send_frame_vc(). It is not redundant: this
             * branch has side effects that are not sends -- it learns
             * dv.local_conid into ps->dir_remote_conid and conn_bind()s the
             * directory CDT -- and letting those run on a CLOSED circuit
             * re-binds a connection whose SYSAP was just told the circuit is
             * gone. The choke point stops the FRAMES; this stops the STATE. */
            if (dv.is_dir_connect_request && !ps->dir_connected &&
                !scsd_refuse_without_open_vc(ps, ps->pb,
                                             "SCS$DIRECTORY CONNECT-RESPONSE", NULL)) {
                /*
                 * vms-7fe: THE p. 2-48 SCAN COMES FIRST. "When a CONNECT_REQ is
                 * received on a VAX system, SCS scans this queue looking for an
                 * SDIR containing a SYSAP name matching the target name
                 * contained in the connect request." The target name is the
                 * GROUNDED 16-byte field at payload [62:78] (spec sec 4(h)(2)).
                 * A miss refuses inside the helper and this branch then does
                 * nothing at all. (The helper's OTHER refusal, p. 2-50's busy
                 * listener, cannot arise here: this branch answers
                 * synchronously, so no listening CDT is in CONNECT RECEIVED
                 * when the next frame arrives -- scs_sdir.h DESIGN CHOICE 3,
                 * measured by test_scsd_wire.c's end-of-run busy total.)
                 *
                 * WHAT REACHES THIS BRANCH, AND HOW NEAR-CERTAIN THE HIT IS.
                 * scs_dir_parse() only sets is_dir_connect_request when the
                 * name field's FIRST 13 BYTES are "SCS$DIRECTORY" (vms-246,
                 * scs_dir.c) -- so for every frame the reference VAX sends, the
                 * name is exactly "SCS$DIRECTORY", a name OVMX LISTENs for, and
                 * THE SCAN HITS. It is not unconditional, though, and the
                 * difference is not academic: the classifier compares 13 bytes
                 * of a 16-byte field while the scan looks up the whole trimmed
                 * name, so a frame naming e.g. "SCS$DIRECTORYX" is classified
                 * here and MISSES -- and is now refused instead of silently
                 * binding the directory connection, which is what this branch
                 * did before this item.
                 *
                 * COVERAGE, and this comment is the second attempt at it: the
                 * entire guard below was once deleted with the whole vmsscs
                 * suite still green. tests/vmsscs/test_scsd_wire.c cases
                 * (2c)/(2d)/(2e) now cover the hit, the miss and the p. 2-50
                 * state read/restore on THIS branch; deleting the guard reds 21
                 * checks (measured, 2026-08-05).
                 *
                 * WHAT THAT COVERAGE IS AND IS NOT, because the bound above
                 * limits it: (2c)'s hit is the ONLY one of the three that runs
                 * on a captured frame. (2d)'s miss needs the substituted name
                 * -- no frame a real VAX sends reaches this branch and misses,
                 * so the refusal wire shape is pinned but never observed, and
                 * its status word is an OVMX invention (spec sec 5). (2e)'s
                 * arrangement -- a listening CDT already in CONNECT RECEIVED --
                 * THIS LOOP CANNOT PRODUCE at all, so it does not guard the
                 * synchronous answer it depends on. And no case asserts a
                 * p. 2-50 busy frame, because none is emittable here.
                 */
                const struct scs_sdir *dsdir = NULL;
                if (scsd_sdir_live() &&
                    !scsd_sdir_admit(rx->sock, rx->ifindex, rx->our_hw_mac,
                                     rx->our_src_logical, ps, buf, (size_t)n,
                                     dv.opcode, dv.local_conid, "SCS$DIRECTORY",
                                     &dsdir)) {
                    return;
                }

                /* Learn the peer's SCS$DIRECTORY handle (its local Con.ID),
                 * then reply op=1 CONNECT-ECHO + op=2 CONNECT-RESPONSE. Each
                 * is a sequenced message: advance OVMX's send_seq per frame
                 * (spec sec 4h(4)). */
                ps->dir_remote_conid = dv.local_conid;
                /* vms-dd5: OVMX is the TARGET on this connection -- Figure
                 * 2-14's NODE_2 column, whose four messages are on the
                 * wire as connection-control message types 0/1/2/3
                 * (docs/cluster-protocol-spec.md sec 4(h)(1a), GROUNDED over
                 * 16 dialogues). This frame is message type 0 = CONNECT_REQ:
                 * 110 bytes, destination Con.ID 0, SYSAP name present. */
                struct scs_dir_params dp;
                memset(&dp, 0, sizeof(dp));
                memcpy(dp.dst_mac, ps_port_addr(ps), 6);
                memcpy(dp.src_mac, rx->our_hw_mac, 6);
                memcpy(dp.src_logical, rx->our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
                memcpy(dp.peer_logical, ps_sys_addr(ps), 6);
                dp.remote_conid = ps->dir_remote_conid;
                dp.local_conid = PS_SCS_DIR_CONID(ps);
                /* vms-246 (§4i established-join): echo the member's current
                 * node-incarnation into the directory [22:24], the same
                 * value it stamps in its own 0x5b connect-request and its
                 * 0x41 START (§4i.B). Read off the wire (ps->incarnation);
                 * 0 leaves the fresh template value 1. */
                dp.incarnation = ps->incarnation;

                /* vms-561: THE ACCEPT SERVICE (p. 2-23, p. 2-56). One call
                 * drives Figure 2-14's whole NODE_2 column -- CLOSED
                 * --RCV_CONNECT_REQ--> CONNECT REC (emitting the op=1
                 * CONNECT-ECHO, which is the CONNECT_RSP) and CONNECT REC
                 * --SVC_ACCEPT--> ACCEPT SENT (emitting the op=2
                 * CONNECT-RESPONSE, which is the ACCEPT_REQ). The CDT, the
                 * p. 2-28 VC-loss handler and both transitions belong to the
                 * service; scsd_svc_emit_dir_accept() above builds and sends
                 * the two frames, in that order, with the same per-frame
                 * send_seq advance as before. */
                struct scsd_svc_emit_ctx ec;
                memset(&ec, 0, sizeof(ec));
                ec.sock = rx->sock;
                ec.ifindex = rx->ifindex;
                ec.ps = ps;
                ec.vc = ps->pb;
                ec.dir_params = &dp;
                /* vms-578: worktree-760's ONLY behavioural delta here was to
                 * MIRROR the requester's msgtype into both answer frames
                 * (abs 30) -- an established member probes with the DATA-phase
                 * 0x4b and the reference VAX answers 0x4b, where the templates
                 * emit the 0x5b establishing form. The vms-561 ACCEPT service
                 * owns the emit now, so the mirror is carried on the emit
                 * context and applied in scsd_svc_emit_dir_accept(). */
                ec.peer_msgtype = buf[30];

                struct scs_svc_args da;
                memset(&da, 0, sizeof(da));
                da.local_sysap = "SCS$DIRECTORY";
                da.remote_sysap = "SCS$DIRECTORY";
                da.vc = ps->pb;
                da.cfg = rx->cfg;
                da.target_node = ps_sys_addr(ps);
                da.vc_loss = scsd_sysap_vc_loss;
                /* vms-7c0: p. 2-29 -- the message input routine is an argument to CONNECT
                 * and ACCEPT, and this is the first OVMX build that supplies one. No
                 * datagram input routine is supplied; see the note under
                 * scsd_sysap_msg_input() for the measurement that says why. */
                da.msg_input = scsd_sysap_msg_input;
                da.sysap_ctx = ps;
                da.conid = PS_SCS_DIR_CONID(ps);
                da.remote_conid = ps->dir_remote_conid;
                da.emit = scsd_svc_emit_dir_accept;
                da.emit_ctx = &ec;

                struct scsd_svc_mark dmark = scsd_svc_mark();
                struct scs_cdt *dcdt = NULL;
                enum scs_svc_status dst = scs_accept(scsd_svc(), &da, &dcdt);
                scsd_svc_settle(dmark);
                /* p. 2-50, as OVMX times it (scs_sdir.h DESIGN CHOICE 3): the
                 * listening CDT returns to LISTEN now that the answer has been
                 * emitted -- or abandoned, which is why this is unconditional.
                 * p. 2-49: the local Con.ID that went out is SCS_DIR_OVMX_CONID,
                 * the SEPARATE CDT's, never dsdir's listening Con.ID. */
                scs_sdir_connect_answered(scs_svc_sdir_mut(scsd_svc()), dsdir);
                if (dst == SCS_SVC_NOCDT) {
                    scsd_svc_slot_refused(PS_SCS_DIR_CONID(ps), "SCS$DIRECTORY");
                }
                if (dcdt != NULL) {
                    ps->cdt_dir = dcdt;
                }
                if (ec.sent) {
                    ps->dir_connected = 1;
                    rx->dir_conn_resp_sent++;
                    log_ts(stdout);
                    printf(" SCSD-I-DIRCONN, bound SCS$DIRECTORY: remote=0x%08X"
                           " local=0x%08X with peer %02x:%02x:%02x:%02x:%02x:%02x\n",
                           ps->dir_remote_conid, (unsigned)PS_SCS_DIR_CONID(ps),
                           ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                           ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5]);
                    fflush(stdout);
                    /* vms-d94: the post-START directory phase has begun --
                     * PROMPTLY open OUR VMS$VAXcluster connection to the
                     * member (clean-ref idx52) before its CM times out and
                     * re-issues START.
                     *
                     * vms-760: ...UNLESS the join SEQUENCER is enabled. It owns
                     * every joiner-initiated connect, in strict lookup-gated
                     * order off OVMX's own dir connection; firing the VC connect
                     * here before its VMS$VAXcluster lookup resolves is the 760b
                     * bug. With the sequencer OFF (the default) this stays the
                     * proven active-joiner path.
                     *
                     * vms-398: the caller names the NODE the START completed
                     * with, not a circuit, so CONNECT selects the virtual
                     * circuit itself via CONFIG_SYS (p. 2-47). */
                    if (!rx->join_seq_enabled && ps->start_acked &&
                        !ps->joiner_connected &&
                        send_joiner_connect_request(rx->sock, rx->ifindex, rx->cfg, ps, NULL,
                                                    rx->our_hw_mac, rx->our_src_logical)) {
                        rx->connect_req_sent++;
                        log_ts(stdout);
                        printf(" SCSD-I-CONNREQ, sent OUR VMS$VAXcluster CONNECT-REQUEST"
                               " local_conid=0x%08X seq=%u (active joiner, prompt)\n",
                               PS_JOINER_CONID(ps), ps->joiner_req_seq);
                        fflush(stdout);
                    }
                }
            } else if (dv.is_lookup_request) {
                /* Answer the SYSAP-name lookup. OVMX serves ONLY the
                 * VMS$VAXcluster connection manager; affirm it, refuse the
                 * rest with the GROUNDED "NOT PRESENT HERE" marker. */
                struct scs_dir_lookup_params lp;
                memset(&lp, 0, sizeof(lp));
                memcpy(lp.dst_mac, ps_port_addr(ps), 6);
                memcpy(lp.src_mac, rx->our_hw_mac, 6);
                memcpy(lp.src_logical, rx->our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
                memcpy(lp.peer_logical, ps_sys_addr(ps), 6);
                lp.remote_conid = ps->dir_remote_conid ? ps->dir_remote_conid
                                                       : dv.local_conid;
                lp.local_conid = PS_SCS_DIR_CONID(ps);
                lp.recv_ack = ps->vc.seq.recv_seq;
                lp.send_seq = scs_seq_advance(&ps->vc.seq);
                lp.incarnation = ps->incarnation; /* §4i established-join echo (see connect branch) */
                /* vms-2f3 sec 4M: do not mirror the request msgtype.
                 * ⚠ AN OVMX DESIGN CHOICE, NOT A REFERENCE RULE -- sec 4M.12
                 * refuted the "a real VAX never mirrors" justification this
                 * originally carried, and the live candidate keys on the
                 * RESULT (present -> 0x4b, absent -> 0x5b) rather than on the
                 * request. See scs_dir_response_msgtype() for the census.
                 * It is NOT the rejoin gate either: four rejoins were refused
                 * with it on, between joining controls (sec 4M.6/4M.9).
                 * OVMX_DIR_MIRROR_MSGTYPE=1 restores the old mirror so the
                 * failing case stays reproducible (guardrail 21). */
                lp.opcode = scs_dir_response_msgtype(
                    dv.opcode, getenv("OVMX_DIR_MIRROR_MSGTYPE") != NULL);
                lp.op = dv.op;
                memcpy(lp.name, dv.name, SCS_DIR_NAME_LEN);
                /*
                 * vms-7fe: THE ANSWER NOW COMES FROM THE SDIR QUEUE.
                 *
                 * p. 2-22: "The SCS Directory Service is in fact a SYSAP that
                 * responds to inquires from other nodes wanting to know if
                 * particular SYSAP names are in this list." Until this item the
                 * "list" was a hardcoded `memcmp(dv.name, "VMS$VAXcluster", 14)`
                 * right here -- an affirmative table of one, which could not
                 * disagree with what OVMX actually served and, in the
                 * SCS$DIRECTORY case, did: it answered NOT PRESENT HERE for a
                 * SYSAP the daemon was serving in the same breath.
                 *
                 * WIRE EFFECT, stated exactly. The only queried name whose
                 * answer changes is SCS$DIRECTORY (now affirmative). Every other
                 * name the reference VAX asks about -- MSCP$TAPE, MSCP$DISK --
                 * is not in the queue and still gets the GROUNDED 16-byte
                 * "NOT PRESENT HERE" marker at [78:94] (spec sec 4(h)(2)), byte
                 * for byte as before, because OVMX LISTENs for neither and
                 * deliberately does not (see scsd_svc()).
                 *
                 * OVMX_NO_SDIR restores the old compare verbatim.
                 */
                if (scsd_sdir_live()) {
                    lp.affirmative = scs_sdir_lookup(scs_svc_sdir_mut(scsd_svc()),
                                                     dv.name) != NULL;
                } else {
                    lp.affirmative = (memcmp(dv.name, "VMS$VAXcluster", 14) == 0);
                }
                /* vms-578: THE MSCP$DISK HIT, carried over from worktree-760,
                 * and KNOWN to disagree with the SDIR queue above.
                 *
                 * worktree-760's grounding (NOT re-measured by this item): the
                 * member's established-join choreography resolves MSCP$DISK on
                 * the joiner and does not open the MSCP$DISK connect without
                 * the HIT. The merged tree DOES answer that connect
                 * (scs_dir_build_mscp_echo / _mscp_accept), so the affirmative
                 * is true about the CONNECTION -- but OVMX serves no MSCP
                 * COMMANDS, which is why MSCP$DISK is not in the LISTEN set and
                 * why this is a second source of truth for one wire answer.
                 * The full argument, and why it is an escalation rather than a
                 * decision, is in the scsd_svc() note.
                 *
                 * OVMX_DISKLESS=1 withdraws it (join as a satellite serving no
                 * disk) -- kill switch for the whole paragraph.
                 *
                 * The per-name RESULT descriptor is selected inside
                 * scs_dir_build_lookup_response: MSCP$DISK echoes the queried
                 * name, VMS$VAXcluster gets the SCA#38 blob. MSCP$TAPE stays a
                 * miss under both branches. */
                /* vms-578: NO second source of truth. worktree-760 affirmed
                 * MSCP$DISK with a hardcoded compare here while the LISTEN set
                 * omitted it -- exactly the split vms-7fe removed. The name is
                 * now registered (or not) by ovmx_mscp_server_enabled() and
                 * this answer follows the queue, whichever way the flag is set.
                 * The per-name RESULT descriptor is still selected inside
                 * scs_dir_build_lookup_response: MSCP$DISK echoes the queried
                 * name, VMS$VAXcluster gets the SCA#38 blob. */
                uint8_t lframe[SCS_DIR_LOOKUP_FRAME_LEN];
                if (scs_dir_build_lookup_response(&lp, lframe) == 0 &&
                    send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                                  "SCS$DIR_LOOKUP response", lframe,
                                  sizeof(lframe)) > 0) {
                    ps->dir_lookups_answered++;
                    rx->dir_lookup_sent++;
                    log_ts(stdout);
                    /* vms-2f3 sec 4M: print BOTH msgtypes. This line used to
                     * print dv.opcode alone, labelled "op=", which reads as
                     * our answer and is actually the REQUEST's msgtype. That
                     * mislabelling hid the rejoin discriminator for four
                     * sessions -- the datum was in every log the whole time. */
                    printf(" SCSD-I-DIRLOOKUP, resolved '%s' -> %s"
                           " (req msgtype=0x%02x, our resp msgtype=0x%02x)\n",
                           dv.name, lp.affirmative ? "AFFIRMATIVE" : "NOT PRESENT HERE",
                           dv.opcode, lp.opcode);
                    fflush(stdout);
                }
            }
            return;
        }
    }

    /* (c) SCS envelope directed to us -> inspect; complete the connect. */
    if (rx->do_connect && (cls == SCS_CLASS_OTHER || cls == SCS_CLASS_SCS_FIXED)) {
        struct scs_connect_view v;
        if (scs_connect_parse(buf, (size_t)n, &v) != 0) {
            return;
        }
        log_ts(stdout);
        printf(" SCSD-I-SCSENV, msgtype=0x%02x fmt=0x%02x len=%u"
               " remote_conid=0x%08X local_conid=0x%08X\n",
               v.msgtype, v.format, v.total_sca_len, v.remote_conid, v.local_conid);
        fflush(stdout);

        /* vms-45c (rung-ADD): a JOINER-INITIATED VMS$VAXcluster connect is
         * msgtype 0x5b (SCS_MSGTYPE_DIRLOOKUP's value; the VC is not up yet, so
         * it rides the establishing 0x5b form -- GROUNDED, scsd_svc_emit_
         * connect_req / vax3-2to3-established-join). OVMX's member-accept below
         * only ever admitted the member's 0x4b (SEQAPP) form, because a VAX
         * joiner never connects TO OVMX -- OVMX joins the VAX. Two OVMX nodes
         * are symmetric joiners, so each must ACCEPT the OTHER's 0x5b joiner
         * connect (send the ACCEPT_REQ the peer waits on) to reach JOINBOUND /
         * ADD_MEMBER. Under the member-role flag, also admit the 0x5b connect;
         * the length gate just below scopes it to the 110-byte connect / 190-
         * byte add-member classes (a 0x5b directory LOOKUP is 62/66-byte and is
         * handled on the dir Con.ID branches long before it could reach here).
         * Flag-off, only 0x4b is accepted -- byte-identical toward a real VAX,
         * which drives the accept in its own 0x4b form. */
        int connect_msgtype_ok =
            (v.msgtype == SCS_MSGTYPE_SEQAPP) ||
            (v.msgtype == SCS_MSGTYPE_DIRLOOKUP && scsd_member_initiate_enabled());
        if (!connect_msgtype_ok || !v.has_conid) {
            return;
        }
        /* vms-770 (vms-a61 audit): has_conid used to BE the length-class test
         * this branch needs -- scs_connect_parse only set it for the 110-/190-
         * byte classes, so "SEQAPP + has_conid" meant "a CONNECT/ACCEPT-class
         * frame" for free. vms-a61 widened has_conid to every envelope-
         * conformant class (58/62/66/86/94 too, scs_env.h), which is the
         * correct fix THERE (the Con.ID pair is exactly as grounded on those
         * classes) but silently deleted the length-class guarantee this branch
         * was relying on. Left alone, a short SEQAPP data/credit frame (e.g.
         * the 58-content class, scs_credit.h's "0x4B13 family") whose
         * destination Con.ID happens to be 0 would fall into the
         * CONNECT-RESPONSE completion dialogue below and OVMX would transmit
         * a bogus CONNECT-ECHO/CONNECT-RESPONSE answering a frame that was
         * never a CONNECT_REQ. Restore the scoping explicitly instead of
         * leaning on has_conid's old side effect: only the 110-byte
         * CONNECT/ACCEPT class and the 190-byte add-member class carry a
         * connect handshake this branch knows how to complete. */
        if (v.total_sca_len != SCS_CONNECT_SCA_LEN &&
            v.total_sca_len != SCS_MEMBER_SCA_LEN) {
            return;
        }
        struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
        if (ps == NULL) {
            return;
        }
        if (v.remote_conid == 0) {
            /* vms-c6d: the ESTABLISHED VAX drives the VMS$VAXcluster 0x4b
             * CONNECT-REQUEST (remote Con.ID still 0 = OVMX's not yet known)
             * and OVMX answers with a CONNECT-RESPONSE echoing the VAX's
             * Con.ID and supplying its own -- the admission act that binds
             * the CDT (spec sec 4g phase 4). The response MUST carry OVMX's
             * LIVE VC counters: recv_ack = recv_seq (acks the VAX's just-sent
             * request send_seq, already note_recv'd by the credit block) and
             * a freshly-advanced send_seq. Baking the golden 7/8 made the VAX
             * reject the accept and retransmit forever. The VAX retransmits
             * its request until it accepts our response, so RE-ANSWER each
             * request (a new sequenced message with current counters) instead
             * of going silent once bound -- a lost response then self-heals. */
            int first = !ps->connected;
            /* vms-760 server-first: the member-opened VC is accepted with an
             * op=1 CONNECT-ECHO *before* the op=2 CONNECT-RESPONSE (every accept
             * in this protocol echoes first; af2 VC 143.7586 op=1 -> 143.7587
             * op=2). Echo only on FIRST bind; re-answers below just re-send the
             * op=2. Skipping the echo is why the member never bound OVMX's VC
             * and withheld its config.
             *
             * vms-e81: THE ENV GATE IS GONE, AND ITS ABSENCE COST A DAY. The
             * echo was correct, written, commented -- and reachable only with
             * OVMX_PURE_SERVER set, which no normal run sets. The comment
             * above even states the consequence of skipping it. Meanwhile the
             * bystander investigation chased ordering, patience, incarnation
             * and membership propagation, because from the outside the symptom
             * was 'the newcomer ignores us'.
             *
             * It does not ignore us -- it is BLOCKED ON US. When VAX3 opened
             * its VMS$VAXcluster connect to OVMX we answered op 0 -> op 2,
             * skipping op 1, so VAX3 withheld its op-3 CONFIRM, the VC stayed
             * half-open, VAX3 never ran the add-member dialogue with us and
             * therefore never sent op 0x02 to the coordinator AT ALL. It sat
             * at NEW for 280 s. Our own join path never exposed this because
             * there OVMX opens the VC and the member accepts -- the accept
             * side barely runs.
             *
             * Correct sequencing (control idx 3701/3703/3705): echo consumes
             * req_seq+1, accept consumes req_seq+2. Skipping the echo also
             * left our send_seq one short of what the peer expects. */
            if (first) {
                struct scs_dir_params ep;
                memset(&ep, 0, sizeof(ep));
                memcpy(ep.dst_mac, ps_port_addr(ps), 6);
                memcpy(ep.src_mac, rx->our_hw_mac, 6);
                memcpy(ep.src_logical, rx->our_src_logical, 6);
                memcpy(ep.peer_logical, ps_sys_addr(ps), 6);
                ep.remote_conid = v.local_conid; /* member's VC handle (echoed) */
                ep.local_conid = 0;
                ep.incarnation = ps->incarnation;
                ep.recv_ack = ps->vc.seq.recv_seq;
                ep.send_seq = scs_seq_advance(&ps->vc.seq);
                uint8_t eframe[SCS_DIR_ECHO_FRAME_LEN];
                if (scs_dir_build_vc_echo(&ep, eframe) == 0) {
                    send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                                  "op=1 CONNECT-ECHO",
                                  eframe, sizeof(eframe));
                }
            }
            struct scs_connect_params cp;
            memset(&cp, 0, sizeof(cp));
            memcpy(cp.dst_mac, ps_port_addr(ps), 6);
            memcpy(cp.src_mac, rx->our_hw_mac, 6);
            memcpy(cp.src_logical, rx->our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
            memcpy(cp.peer_logical, ps_sys_addr(ps), 6);
            cp.local_conid = PS_LOCAL_CONID(ps);
            cp.remote_conid = v.local_conid;
            cp.recv_ack = ps->vc.seq.recv_seq;
            cp.send_seq = scs_seq_advance(&ps->vc.seq);
            cp.incarnation = ps->incarnation; /* §4i.B established-join echo (0 => fresh 1) */

            /* vms-561: THE ACCEPT SERVICE again, on the member-opened
             * connection. OVMX is the TARGET here, and one emitted frame
             * covers two transitions:
             *   RCV_CONNECT_REQ -- the member's 0x4b with destination Con.ID
             *     0, message type 0 (spec sec 4g phase 4, golden frame 47).
             *     The machine requires a CONNECT_RSP and OVMX BUILDS NONE, so
             *     scsd_svc_emit_member_accept() answers NOBUILDER and the
             *     SCSD-W-CONNNOACT line records it every run. An OVMX gap, not
             *     a wire gap: the real VAX does send that frame.
             *   SVC_ACCEPT -- the 110-byte CONNECT-RESPONSE (golden frame 50,
             *     message type 2 = ACCEPT_REQ, sec 4(h)(1a)).
             *
             * `first` is read BEFORE the service runs, and drives
             * args.retransmit: a member that re-sends its CONNECT-REQUEST gets
             * the labeled OVMX retransmit row (ACCEPT SENT -> ACCEPT SENT,
             * repeating the ACCEPT_REQ), not a second ACCEPT.
             *
             * vms-561 ALSO ADDS THE UP-FRONT CIRCUIT REFUSAL that the
             * SCS$DIRECTORY branch has carried since vms-abc, and for the same
             * reason: send_frame_vc() stops the FRAME, but by then ACCEPT has
             * already allocated a descriptor and advanced a state for a
             * connection whose circuit is gone. The choke point stops the
             * frames; this stops the state. It costs no extra refusal -- the
             * send that used to be refused inside send_frame_vc() is the one
             * this refuses instead, so vc_sends_refused moves by the same 1. */
            /* vms-578: `first` is declared once, above, by the vms-760 op=1 CONNECT-ECHO block. */
            if (scsd_refuse_without_open_vc(ps, ps->pb,
                                            "VMS$VAXcluster 0x4b CONNECT-RESPONSE",
                                            NULL)) {
                return;
            }

            /*
             * vms-7fe: the p. 2-48 scan on the OTHER inbound CONNECT_REQ. The
             * target name at payload [62:78] reads "VMS$VAXcluster" here, which
             * OVMX LISTENs for, so this HITS and the accept below is unchanged.
             * A retransmitted request carries the same requester Con.ID and is
             * re-delivered rather than refused busy (scs_sdir.h DESIGN CHOICE
             * 4) -- which matters here more than anywhere, because the
             * established VAX retransmits this exact frame until it accepts our
             * answer and `first` is already false on every repeat. Note what
             * that sentence does NOT say: a busy refusal is not merely avoided
             * here, it is unreachable from this loop at all, because the answer
             * below is synchronous (DESIGN CHOICE 3) and the SDIR is back in
             * LISTEN before the next frame is read -- which test_scsd_wire.c
             * measures with an end-of-run busy total, not a per-case check.
             */
            const struct scs_sdir *msdir = NULL;
            if (scsd_sdir_live() &&
                !scsd_sdir_admit(rx->sock, rx->ifindex, rx->our_hw_mac,
                                 rx->our_src_logical, ps, buf, (size_t)n,
                                 v.msgtype, v.local_conid, "VMS$VAXcluster",
                                 &msdir)) {
                return;
            }

            struct scsd_svc_emit_ctx mec;
            memset(&mec, 0, sizeof(mec));
            mec.sock = rx->sock;
            mec.ifindex = rx->ifindex;
            mec.ps = ps;
            mec.vc = ps->pb;
            mec.connect_params = &cp;

            struct scs_svc_args ma;
            memset(&ma, 0, sizeof(ma));
            ma.local_sysap = "VMS$VAXcluster";
            ma.remote_sysap = "VMS$VAXcluster";
            ma.vc = ps->pb;
            ma.cfg = rx->cfg;
            ma.target_node = ps_sys_addr(ps);
            ma.vc_loss = scsd_sysap_vc_loss;
            /* vms-7c0: p. 2-29 -- the message input routine is an argument to CONNECT
             * and ACCEPT, and this is the first OVMX build that supplies one. No
             * datagram input routine is supplied; see the note under
             * scsd_sysap_msg_input() for the measurement that says why. */
            ma.msg_input = scsd_sysap_msg_input;
            ma.sysap_ctx = ps;
            ma.conid = PS_LOCAL_CONID(ps);
            ma.remote_conid = v.local_conid;
            ma.retransmit = !first;
            ma.emit = scsd_svc_emit_member_accept;
            ma.emit_ctx = &mec;

            struct scsd_svc_mark mmark = scsd_svc_mark();
            struct scs_cdt *mcdt = NULL;
            enum scs_svc_status mst = scs_accept(scsd_svc(), &ma, &mcdt);
            scsd_svc_settle(mmark);
            /* p. 2-50 / scs_sdir.h DESIGN CHOICE 3, as above. */
            scs_sdir_connect_answered(scs_svc_sdir_mut(scsd_svc()), msdir);
            if (mst == SCS_SVC_NOCDT) {
                scsd_svc_slot_refused(PS_LOCAL_CONID(ps), "VMS$VAXcluster");
            }
            if (mcdt != NULL) {
                ps->cdt_member = mcdt;
                /* vms-fdd: the ACCEPT service's p. 2-28 copy of the connect
                 * data scs_connect_build_response() stamped. */
                scsd_cdt_record_connect_data(mcdt);
            }
            if (mec.sent) {
                ps->remote_conid = v.local_conid;
                rx->connect_resp_sent++;
                ps->connected = 1;
                log_ts(stdout);
                printf(" SCSD-I-CONNRESP, %s peer VMS$VAXcluster CONNECT-REQUEST:"
                       " remote=0x%08X local=0x%08X recv_ack=%u send_seq=%u incarnation=%u\n",
                       first ? "answered" : "re-answered",
                       v.local_conid, PS_LOCAL_CONID(ps), cp.recv_ack, cp.send_seq,
                       cp.incarnation ? cp.incarnation : 1);
                fflush(stdout);
                /* vms-d94: do NOT send the add-member burst on this
                 * member-opened connection. The clean-ref grounding
                 * (formation-clean-2node.pcap) shows the joiner sends its
                 * add-member config on the connection IT opens (idx52/59),
                 * not on the member-opened one -- OVMX doing the latter is
                 * exactly why VAX1 ignored the burst and looped START. We
                 * still answer the member's CONNECT-REQUEST (above) to keep
                 * that connection alive; the burst rides our own joiner
                 * connection, bound in branch (b1) above. */
                /* vms-760 (same conclusion, independently grounded): the MEMBER
                 * sends its 190-byte config FIRST (af2 t~143.759, member ss=12
                 * before the joiner responds ss=12); OVMX answers with its own
                 * burst in the member-config handler on receipt. Bursting
                 * proactively left the member silent.
                 *
                 * vms-578: worktree-760 also re-added an `else if
                 * (v.remote_conid == OVMX_JOINER_CONID)` bind here. It is NOT
                 * carried over. The closure branch deleted it after MEASURING
                 * that replaying all 19 OVMX lab captures (141,338 frames)
                 * through scsd_handle_frame() runs the (b1) bind 39 times and
                 * this one 0 times, and test_null_source_conid_binds_nothing()
                 * in test_scsd_wire.c reds if it comes back. The one thing that
                 * bind did which (b1) did not was set join_step = JS_ADD_MEMBER;
                 * that assignment is carried into the (b1) bind instead. */
            }
        }
        /* vms-dd5: THERE IS NO `else if (v.remote_conid == OVMX_JOINER_CONID)`
         * BRANCH HERE, and re-adding one would be dead code. Branch (b1)
         * above owns the joiner-accept path, and its guard is IMPLIED by the
         * guard of this block:
         *   - reaching here needs v.msgtype == SCS_MSGTYPE_SEQAPP, i.e.
         *     buf[30] == 0x4b, which is in (b1)'s opcode set;
         *   - reaching here ALSO needs the explicit vms-770 length-class
         *     guard above (v.total_sca_len == 110 or 190) -- has_conid alone
         *     no longer implies this since vms-a61 widened it to every
         *     envelope-conformant class, so this comment's proof now leans on
         *     that guard rather than on has_conid itself -- so (b1)'s
         *     `n >= 72` holds too;
         *   - (b1) RETURNS whenever rconid == OVMX_JOINER_CONID && lconid != 0.
         * So the only frame that could ever have reached a joiner branch here
         * is a 110-/190-byte-class frame whose destination Con.ID is our
         * joiner handle and whose SOURCE Con.ID is 0 -- and binding a
         * connection to a null remote Con.ID is wrong by construction (0 is
         * the "handle not yet assigned" value, spec sec 4(h)(1)). MEASURED,
         * not argued: (a) across all 41 pcaps in the reference-lab capture set
         * there is no 110-/190-class frame with destination Con.ID != 0 and
         * source Con.ID == 0; (b) instrumenting both sites and replaying all
         * 19 OVMX lab captures (141,338 frames) through this very function ran
         * the (b1) site 39 times and this one 0 times, including the
         * MEMBER-achieved success capture. The branch that used to be here was
         * a duplicate of (b1) that never executed; it is deleted, and
         * test_null_source_conid_binds_nothing() in tests/vmsscs/
         * test_scsd_wire.c reds if it comes back. */
        return;
    }
}

/* =====================================================================
 * vms-17f -- PEER DEPARTURE, AND WHY THE p. 2-21 REFRESH WAS UNREACHABLE
 * =====================================================================
 *
 * WHAT WAS BROKEN. scs_config.c has implemented the p. 2-21 Note correctly since
 * vms-7be -- "If there are no other Path Blocks queued to the old System Block
 * ... the old System Block is refreshed based on the contents of the formative
 * System Block ... Typically, this happens when the remote node was once in the
 * cluster, departed, and is now rebooting." SCSD could not reach it, for three
 * reasons that all had to be fixed together:
 *
 *   1. scs_pb_close() had NO CALLER anywhere in this file. A circuit, once open,
 *      stayed open for the life of the process.
 *   2. peer_find_or_add() matches on port address and only ever ALLOCATES. A
 *      slot was never released, so a returning node re-entered its stale one.
 *   3. scs_pb_open() ran behind `if (!ps->start_acked)`, and start_acked was
 *      never reset.
 *
 * So a rejoining VAX landed in the SAME peer slot on the SAME already-OPEN Path
 * Block, took scs_pb_open()'s `!pb->on_pdt` early return, and got
 * SCS_OPEN_EXISTING_SB. The REFRESH branch was structurally unreachable and its
 * log clause said so.
 *
 * WHAT THIS SWEEP DOES. When a peer has been silent past the listen timeout it
 * is declared departed: scs_pb_depart() runs the p. 2-28 ordered teardown (see
 * scs_depart.h) and the peer slot is RELEASED. The System Block survives, by
 * design (p. 2-17). When that node comes back, peer_find_or_add() builds a
 * fresh slot -- start_acked back to 0 with the rest of the zeroed struct -- and
 * a fresh FORMATIVE Path Block, formation runs from CLOSED again, and
 * scs_pb_open() finds the old SB with an empty PB queue. That is the p. 2-21
 * Note, executed.
 *
 * THIS IS WIRE-VISIBLE, and the change is exactly this: a returning peer now
 * gets a full formation dialogue (round-0 START, round-1 STACK, round-2 ack)
 * where before it got nothing, because OVMX thought the circuit was still open.
 * OVMX_NO_PEER_DEPART=1 restores the old behaviour completely -- no teardown, no
 * slot release, no second formation.
 *
 * WHAT IS NOT CLAIMED. This does not fix vms-2f3 (OVMX's own rejoin failure) and
 * must not be read as doing so. It is the wiring that failure's diagnosis
 * depends on: eleven hypotheses about frame content have been falsified there,
 * and the strategy note is explicit that the rejoin is a connection-STATE
 * failure. This gives the state a place to be wrong in. Whether it is the fault
 * is a lab question, not a code-reading question.
 *
 * ===== MEASURED IN THE LAB, 2026-08-04 (lab-2 pods, tests/lab/README.md) =====
 *
 * Method: run SCSD against a live 2-node VMScluster, SIGKILL one node's SIMH
 * process (a hard node failure -- NOT a shutdown, which would be a graceful
 * departure and a different case), leave it dead well past the listen timeout,
 * then reboot it with an UNCHANGED SCSNODE/SCSSYSTEMID. That is the p. 2-21
 * Note's own scenario. Captures and SCSD logs are on the lab volume at
 * /data/training/vax/k8s-labs/<pod>/logs/{17f-<tag>.pcap,scsd-<tag>.log}.
 *
 *   D2  vaxlab-2, VAX1 killed -- the node OVMX had an OPEN circuit and TWO SCS
 *       connections with. SCSD: "peer aa:00:04:00:01:04 silent 20291ms ...
 *       path block CLOSED, peer slot released, 2 connection(s) lost". When VAX1
 *       came back: "path block OPEN, old system block REFRESHED (rejoin,
 *       p. 2-21 Note)" and PB-OPEN: new-sb=1 refreshed=1. THE ITEM'S CLAIM,
 *       on the wire.
 *   D0  vaxlab-3, IDENTICAL scenario with OVMX_NO_PEER_DEPART=1. PB-OPEN:
 *       new-sb=1 refreshed=0, PEER-DEPARTURES=0, zero SCSD-I-PEERGONE lines.
 *       The switch suppresses the behaviour completely (guardrail 23: the
 *       counter was RUN both ways before anything was written down here).
 *   TZ  vaxlab-2, a fresh identity, 150 s, nobody killed. PB-OPEN: new-sb=1,
 *       PEER-DEPARTURES=0 -- the ordinary join is unchanged and no healthy peer
 *       is aged out.
 *
 * EVERY DEPARTURE RE-DERIVED FROM THE RAW CAPTURE, not from SCSD's own log: in
 * D2 the three rx->peers SCSD departed last transmitted at 17:09:23.9 / 17:09:54.7 /
 * (VAX1) 17:07:10.6, and SCSD declared them gone 20.9 s / 20.5 s / 20.3 s later.
 * No peer that was still transmitting was departed. VAX1's silence in that
 * capture is 165.6 s end to end -- the kill, the 95 s dead window and the reboot.
 */

/*
 * scsd_peer_departure_sweep - age every peer slot against `now_ms` and tear down
 * the ones that have gone quiet. Returns the number of rx->peers declared departed.
 *
 * Separated from main()'s loop for the reason vms-fb1 separated the receive
 * dispatch: a timer block inside main() is renamed away by SCSD_UNIT_TEST and
 * can therefore be mutated freely without reddening a test. This is the
 * production sweep and tests/vmsscs/test_scsd_wire.c calls this function.
 *
 * KNOWN LIMIT, stated rather than hidden: main() calls this once per loop
 * iteration, and the loop is driven by recv(). The 1-second SO_RCVTIMEO that
 * makes it wake on an idle wire is set only in --emit-hello mode (which
 * --rx->respond and --connect both imply), so a receive-only SCSD watching a wire
 * that goes COMPLETELY silent blocks in recv() and does not sweep until some
 * frame arrives. Every mode that can form a circuit sets the timeout, so no
 * circuit OVMX actually opens is affected; a listener that opens none has
 * nothing to tear down.
 */
static unsigned scsd_peer_departure_sweep(struct scsd_rx *rx, uint64_t now_ms)
{
    if (rx == NULL || rx->peers == NULL) {
        return 0;
    }
    if (!scs_depart_enabled()) {
        /* THE KILL SWITCH: no peer is ever declared departed.
         *
         * REDUNDANT BY DESIGN, and measured to be: scs_pb_depart() checks the
         * same switch, and the `res != SCS_PB_CLOSE_OK` guard below then
         * declines to release the slot -- so deleting these three lines changes
         * no observable behaviour (mutation M10 of the vms-17f battery survives
         * the whole suite, and is listed as an equivalent mutant rather than
         * quietly dropped). It is kept because the gate belongs at the top of
         * the policy that reads the timeout, not only inside the mechanism. */
        return 0;
    }
    uint64_t timeout_ms = scs_depart_listen_timeout_ms();
    unsigned departed = 0;

    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        struct peer_state *ps = &rx->peers[i];
        if (ps->pb == NULL || ps->last_rx_ms == 0) {
            continue; /* free slot, or one never heard from -- nothing to age */
        }
        if (now_ms < ps->last_rx_ms || now_ms - ps->last_rx_ms < timeout_ms) {
            continue;
        }

        uint8_t gone_mac[6];
        memcpy(gone_mac, ps_port_addr(ps), 6);
        uint64_t silent_ms = now_ms - ps->last_rx_ms;

        /* vms-096: TELL THE POLLER BEFORE THE SWEEP RELEASES ITS DESCRIPTOR.
         * scs_pb_depart() releases every CDT queued to this Path Block, and
         * the poller's in-flight cycle connection is an ordinary CDT on it.
         * scs_depart.c sits UNDER scs_poll.c in the library graph and cannot
         * make this call itself. Without it the poller keeps a pointer to a
         * released slot in a non-IDLE state, and the next scs_poll_abandon()
         * releases whatever has since been allocated into that CDL slot. Must
         * precede the sweep: afterwards the CDT is memset and its `pb` link --
         * the only way to recognise it as being on this circuit -- is gone.
         *
         * `scsd_poller_ready ? &scsd_poller : NULL` and NOT scsd_poll(rx):
         * scsd_poll() BINDS the poller on first use, registering its SYSAP name
         * with the port. A departure sweep must not bring a poller into
         * existence that the daemon never asked for -- measured, doing so left
         * a descriptor in the CDL and reddened
         * test_rejoin_reaches_the_p221_refresh's "no connection survived the
         * departure" assertion. If no poller was ever bound it has no cycle to
         * lose, so NULL is exactly right. */
        (void)scs_poll_pb_departing(scsd_poller_ready ? &scsd_poller : NULL, ps->pb);

        struct scs_depart_stats st;
        enum scs_pb_close_result res =
            scs_pb_depart(scsd_cdl_ready ? &scsd_cdl : NULL, rx->cfg, ps->pb, &st);

        if (res == SCS_PB_CLOSE_CONNECTIONS_QUEUED) {
            /* A SYSAP error handler queued a NEW connection to the dying
             * circuit. Nothing here can safely destroy a structure a SYSAP is
             * holding, so the slot is LEFT ALONE and re-aged next sweep. Loud,
             * because it means the teardown did not complete (INV-6: never fake
             * success for an executive facility). */
            depart_refusals++;
            log_ts(stderr);
            fprintf(stderr,
                    " SCSD-W-DEPARTBUSY, peer %02x:%02x:%02x:%02x:%02x:%02x silent"
                    " %llums but its path block still carries %u connection(s) --"
                    " NOT torn down, will retry\n",
                    gone_mac[0], gone_mac[1], gone_mac[2], gone_mac[3], gone_mac[4],
                    gone_mac[5], (unsigned long long)silent_ms, scs_pb_cdt_count(ps->pb));
            fflush(stderr);
            continue;
        }
        if (res != SCS_PB_CLOSE_OK) {
            /* The teardown did not happen -- the kill switch is set, or the PB
             * was not in use. Releasing the slot anyway would be a departure
             * that tore nothing down, i.e. exactly the "report success for work
             * that was not done" shape INV-6 forbids. Leave it alone. */
            continue;
        }

        peer_departures++;
        depart_connections_lost += st.connections_lost;
        departed++;

        log_ts(stdout);
        printf(" SCSD-I-PEERGONE, peer %02x:%02x:%02x:%02x:%02x:%02x silent %llums"
               " (listen timeout %llums) -- path block CLOSED, peer slot released,"
               " %u connection(s) lost, %u credit waiter(s) flushed,"
               " %u MFREEQ buffer(s) returned, %u DFREEQ buffer(s) returned;"
               " its system block STAYS in the"
               " configuration queue (p. 2-17) so a rejoin can refresh it (p. 2-21)."
               " Configuration queue now holds %u system block(s)\n",
               gone_mac[0], gone_mac[1], gone_mac[2], gone_mac[3], gone_mac[4],
               gone_mac[5], (unsigned long long)silent_ms,
               (unsigned long long)timeout_ms, st.connections_lost,
               st.waiters_flushed, st.mfreeq_reclaimed, st.dfreeq_reclaimed,
               scs_config_sb_count(rx->cfg));
        fflush(stdout);

        /* Release the slot, WHOLE. `pb = NULL` alone would be enough to free it
         * for reuse (peer_find_or_add zeroes any slot it allocates), but a free
         * slot must not sit there holding a departed node's connection handles,
         * incarnation and start_acked: the only thing that distinguishes free
         * from in-use is that one pointer, and every other field would read as
         * the returning node's if anything ever looked without checking it.
         * The memset is also what resets start_acked (and the three CDT
         * pointers, whose CDTs scs_pb_depart just released), so the
         * returning node re-drives formation from CLOSED instead of being pinned
         * to state from its previous incarnation. */
        memset(ps, 0, sizeof(*ps));
    }
    return departed;
}

/*
 * scsd_vc_reissue_tick - vms-4071's p. 2-14 formation reissue timer. "whenever
 * a port driver sends a START or a STACK ... it starts a timer and expects a
 * response. If the timer expires before any response is received ... SCA
 * requires the port driver to reissue the START or STACK (whichever it last
 * sent)", bounded by an "operating system dependent retry limit" after which
 * "formation of the virtual circuit is to be abandoned". Returns the number of
 * reissues (SEND_START/SEND_STACK actions taken) on this tick.
 *
 * WHY IT IS A FUNCTION (vms-fb1). This was an inline block in main()'s loop,
 * and SCSD_UNIT_TEST renames main() away, so it was compiled but reachable
 * from no test -- the same measured gap vms-abc closed for the retransmit tick
 * and vms-ebb closed for the disk-discovery ungate. The body is that block,
 * moved verbatim: `vc_ctx` becomes `rx->vc_ctx` (main() already points it at
 * the same struct via `rx.vc_ctx = &vc_ctx`), `peers` becomes `rx->peers`, and
 * the loop-local `rx.start_sent`/`rx.start_ack_sent` become `rx->`. No guard,
 * no order and no log line changed.
 *
 * OVMX_VC_NO_RETRY_LIMIT=1 (read inside scs_vc_retry_limit()) restores
 * unbounded retry with no abandon; unchanged by this move.
 */
static unsigned scsd_vc_reissue_tick(struct scsd_rx *rx, uint64_t now_ms)
{
    if (rx == NULL || !rx->do_connect) {
        return 0;
    }
    unsigned reissued = 0;
    unsigned retry_limit = scs_vc_retry_limit();
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        struct peer_state *ps = &rx->peers[i];
        if (ps->pb == NULL) {
            continue;
        }
        if (!scs_vc_fsm_timer_expired(ps->pb, now_ms, SCS_VC_FORMATION_TIMEOUT_MS)) {
            continue;
        }
        enum scs_vc_action act = scs_vc_fsm_timeout(ps->pb, now_ms, retry_limit);
        if (act == SCS_VC_ACT_SEND_START || act == SCS_VC_ACT_SEND_STACK) {
            if (scsd_vc_emit(rx->vc_ctx, ps, act)) {
                rx->start_sent++;
                ps->start_replies++;
                reissued++;
                log_ts(stdout);
                printf(" SCSD-I-VCREISSUE, formation timer expired -- %s to peer"
                       " %02x:%02x:%02x:%02x:%02x:%02x (attempt %u, VC %s)\n",
                       scs_vc_action_name(act),
                       ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                       ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
                       ps->pb->fsm.retries, scs_vc_state_name(ps->pb->vc_state));
                fflush(stdout);
            }
        }
        /* peer_round2_seen=0: a timer expiry is not a peer frame. */
        scsd_vc_settle(rx->vc_ctx, ps, act, 0, &rx->start_ack_sent);
    }
    return reissued;
}

/*
 * scsd_poll_refresh_tick - vms-66f's p. 2-50 SCS process poll. "A VMS system
 * will poll each other node that it can see approximately once, but not more
 * than once, during each such interval" -- so the set of nodes it can see is
 * refreshed here, from the ONE authority on whether a circuit is open
 * (CONFIG_PATH, p. 2-47), and scs_poll_tick() decides whether anything
 * happens.
 *
 * WHY IT IS A FUNCTION (vms-fb1). This was an inline block in main()'s loop,
 * and SCSD_UNIT_TEST renames main() away, so it was compiled but reachable
 * from no test -- the same measured gap vms-abc closed for the retransmit tick
 * and vms-ebb closed for the disk-discovery ungate. The body is that block,
 * moved verbatim: `peers` becomes `rx->peers`, `rx.cfg` becomes `rx->cfg`.
 *
 * ASK THE QUESTION CONNECT WILL ASK, not a similar one -- see the comment on
 * scs_config_select_vc() below, unchanged from the inline block: the first
 * revision tested the Path Block's own vc_state and MEASURED WRONG on the lab
 * wire (the VC formation machine reaches OPEN one received frame BEFORE
 * scs_pb_open() moves the Path Block off the PDT formative queue).
 *
 * OVMX_NO_PROCESS_POLLER=1 (read inside scs_poll_tick()) forces it to a no-op
 * from any state; unchanged by this move.
 */
static void scsd_poll_refresh_tick(struct scsd_rx *rx, uint64_t now_ms)
{
    if (rx == NULL || !rx->do_connect) {
        return;
    }
    struct scs_poller *poller = scsd_poll(rx);
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        struct peer_state *ps = &rx->peers[i];
        if (ps->pb == NULL) {
            continue;
        }
        const uint8_t *sys = ps_sys_addr(ps);
        if (scs_config_select_vc(rx->cfg, sys) != NULL) {
            (void)scs_poll_add_node(poller, sys);
        } else {
            scs_poll_drop_node(poller, sys);
        }
    }
    scs_poll_tick(poller, now_ms);
}

/*
 * scsd_retransmit_tick - vms-691's retransmit of OVMX's own unacked sequenced
 * message (the CONNECT-REQUEST), plus vms-abc's p. 2-31 DELIVERY guarantee.
 *
 * WHY IT IS A FUNCTION. This was an inline block in main()'s loop, and
 * SCSD_UNIT_TEST renames main() away, so it was compiled but reachable from no
 * test -- the same measured gap vms-fb1 closed for the receive path. The body
 * is that block, moved, with the loop-local state reached through `rx->`.
 *
 * THE BEHAVIOUR CHANGE (vms-abc): reaching the retransmit cap used to be a bare
 * `continue` -- OVMX stopped retransmitting a message the peer had never
 * acknowledged and carried on with the circuit still OPEN. p. 2-31: "if either
 * the guarantee of message delivery or the guarantee of message sequentiality
 * cannot be satisfied, the virtual circuit between the ports involved will be
 * explicitly broken (if it isn't already)." Exhausting the retransmits IS the
 * delivery guarantee failing, so the circuit is now broken instead. The CAP
 * itself is unchanged -- SCS_VC_DELIVERY_RETRY_LIMIT and VC_RETRANSMIT_MAX are
 * both 5 and a _Static_assert above holds them equal -- so the point at which
 * OVMX gives up is exactly where it was; only what it does there changed.
 * OVMX_NO_VC_BREAK=1 restores the limp-on.
 *
 * ===== TWO LOOP BODIES, AND ONLY ONE OF THEM IS LIVE. Read this before
 * changing either guard. =====
 *
 * (A) THE DELIVERY SCAN (vms-abc) is gated ONLY on scs_vc_delivery_failed(),
 *     which is itself gated on `vc.have_unacked`. It is LIVE. OVMX's one
 *     outstanding sequenced message is the vms-d94 joiner CONNECT-REQUEST;
 *     send_joiner_connect_request() is the only LIVE caller of
 *     scs_vc_record_sent()/scs_vc_mark_retransmitted() in the tree (scan (B)
 *     below calls mark_retransmitted too, but (B) is dead), and the
 *     directed-HELLO handler retransmits it at ~1 Hz while the member has not
 *     accepted (see scsd_joiner_retransmit_pending()). So a member that answers
 *     HELLOs but never accepts our connection drives retransmit_count to
 *     SCS_VC_DELIVERY_RETRY_LIMIT within ~5 s and the NEXT main-loop tick
 *     breaks the circuit -- which is the p. 2-31 outcome. THIS IS WHY THE SCAN
 *     IS NOT NESTED UNDER (B)'s GUARD: it must run for every peer with an
 *     outstanding message, not only for the ones (B) would retransmit for.
 *
 * (B) THE vms-691 RETRANSMIT below it -- the one that re-sends a CONNECT-REQUEST
 *     at OVMX_LOCAL_CONID -- is DEAD, and vms-abc deliberately did not revive
 *     it. Its guard tests `ps->connect_sent`, which is ASSIGNED NOWHERE in this
 *     file (`grep -n connect_sent src/vmsscs/scsd.c`: the declaration, this
 *     guard, and two exit-report lines; every assignment in the file is to the
 *     DIFFERENT field `joiner_connect_sent`). It has not run since vms-d94 moved
 *     the connect onto the joiner-opened connection. It is NOT deleted here
 *     because deleting it and reviving it are both wire decisions belonging to
 *     the connect/retransmit path: reviving it would start emitting retransmits
 *     at OVMX_LOCAL_CONID that OVMX does not send today. Reported, not fixed --
 *     see the findings on this item. Nothing in vms-abc's coverage depends on
 *     (B): the delivery break is proved through (A) alone.
 */
static void scsd_retransmit_tick(struct scsd_rx *rx, uint64_t now_ms)
{
    if (rx == NULL || !rx->do_connect) {
        return;
    }
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        struct peer_state *ps = &rx->peers[i];
        if (ps->pb == NULL) {
            continue;
        }
        /* (A) vms-abc: the p. 2-31 DELIVERY guarantee. LIVE -- see the header. */
        if (scs_vc_delivery_failed(&ps->vc)) {
            unsigned broken = scs_vc_break(&ps->vc, ps->pb, SCS_VC_BREAK_DELIVERY, stdout);
            if (scs_vc_break_enabled()) {
                vc_breaks++;
                vc_conns_broken += broken;
                /* vms-c7d: delivery exhaustion is a connectivity loss, not a
                 * last-gasp departure -- enter CSB WAIT, hold membership, let
                 * the RECNXINTERVAL reconnect loop run (transcript p. 7-30). */
                scsd_recnx_note_vc_loss(ps, now_ms);
            }
            continue;
        }
        /* (B) vms-691's retransmit. DEAD -- see the header. */
        if (ps->connected || !ps->connect_sent) {
            continue;
        }
        if (ps->vc.retransmit_count >= VC_RETRANSMIT_MAX) {
            continue;
        }
        if (!scs_vc_retransmit_due(&ps->vc, now_ms, VC_RETRANSMIT_TIMEOUT_MS)) {
            continue;
        }
        struct scs_connect_params cp;
        memset(&cp, 0, sizeof(cp));
        memcpy(cp.dst_mac, ps_port_addr(ps), 6);
        memcpy(cp.src_mac, rx->our_hw_mac, 6);
        memcpy(cp.src_logical, rx->our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
        memcpy(cp.peer_logical, ps_sys_addr(ps), 6);
        cp.local_conid = PS_LOCAL_CONID(ps);
        cp.remote_conid = 0;
        /* vms-c6d: re-send the SAME outstanding sequenced message -- reuse the
         * recorded unacked send_seq (do NOT advance) and the current recv_ack. */
        cp.recv_ack = ps->vc.seq.recv_seq;
        cp.send_seq = ps->vc.unacked_seq;
        cp.incarnation = ps->incarnation;
        /* vms-561: even this DEAD branch goes through the CONNECT service. It
         * is migrated rather than left alone for two reasons: the item's rule
         * is that no site builds a connect frame by hand, and a hand-built one
         * left here is exactly the site a future revival would copy. It reuses
         * scsd_svc_emit_connect_req() unchanged -- same builder, same params,
         * same choke point -- so nothing about it is new except who owns the
         * descriptor. It remains unreachable (`connect_sent` is assigned
         * nowhere) and tools/cluster/scsd_wire_diff.sh does NOT exercise it;
         * that is stated rather than glossed. */
        struct scsd_svc_emit_ctx rec;
        memset(&rec, 0, sizeof(rec));
        rec.sock = rx->sock;
        rec.ifindex = rx->ifindex;
        rec.ps = ps;
        rec.vc = ps->pb;
        rec.connect_params = &cp;

        struct scs_svc_args ra;
        memset(&ra, 0, sizeof(ra));
        ra.local_sysap = "VMS$VAXcluster";
        ra.remote_sysap = "VMS$VAXcluster";
        ra.vc = ps->pb;
        ra.cfg = rx->cfg;
        ra.target_node = ps_sys_addr(ps);
        ra.vc_loss = scsd_sysap_vc_loss;
        /* vms-7c0: p. 2-29 -- the message input routine is an argument to CONNECT
         * and ACCEPT, and this is the first OVMX build that supplies one. No
         * datagram input routine is supplied; see the note under
         * scsd_sysap_msg_input() for the measurement that says why. */
        ra.msg_input = scsd_sysap_msg_input;
        ra.sysap_ctx = ps;
        ra.conid = PS_LOCAL_CONID(ps);
        ra.emit = scsd_svc_emit_connect_req;
        ra.emit_ctx = &rec;

        struct scsd_svc_mark rmark = scsd_svc_mark();
        struct scs_cdt *rcdt = NULL;
        (void)scs_connect(scsd_svc(), &ra, &rcdt);
        scsd_svc_settle(rmark);
        if (rec.sent) {
            scs_vc_mark_retransmitted(&ps->vc, now_ms);
            rx->retransmit_sent++;
            log_ts(stdout);
            printf(" SCSD-I-RETX, retransmit CONNECT-REQUEST to peer"
                   " %02x:%02x:%02x:%02x:%02x:%02x (attempt %u)\n",
                   ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                   ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
                   ps->vc.retransmit_count);
            fflush(stdout);
        }
    }
}

/*
 * scsd_start_initiate_for_peer - vms-d60 (rung-VC): the MEMBER-ROLE 0x41 START
 * INITIATE. Off the main-loop clock, once the NISCA channel to this peer is up
 * (rung-0's directed-HELLO solicit done), OVMX issues its OWN round-0 START to
 * OPEN the virtual circuit -- instead of only REFLECTING a peer's START in the
 * receive path (scsd_handle_frame ~9433, "OVMX issues its own START only when a
 * peer identity-bearing frame arrives"). That reflect-only rule is what makes
 * two symmetric OVMX joiners deadlock: neither initiates, start_acked never
 * latches on either, and the whole join sequencer never fires (rd vms-f3e
 * baseline; the two-OVMX harness stalls at rung-VC).
 *
 * THIS IS THE MEMBER'S ROLE. Against a real VAX, the running MEMBER sends round-0
 * START after it has verified the channel (JOIN_MEMBER_GREET_MS: "a timer, not
 * an event hook", the member acts 0.24-1.67 s after the newcomer's channel is
 * up). OVMX had every RESPONDER half of the choreography but no initiator half.
 * The frame emitted is the EXACT round-0 START the reflect path already builds
 * (scsd_vc_emit -> scs_start_build config-round-0, send_seq=1 grounded joiner
 * value): nothing new goes on the wire, only EARLIER and unprompted.
 *
 * KILL-SWITCH DISCIPLINE (CLAUDE.md Rule 8 / INV-6): fires ONLY when
 * OVMX_MCAST_SOLICIT is set (the member-role umbrella that also arms rung-0) and
 * OVMX_NO_START_INITIATE is unset. Absent the flag, OVMX never initiates -- it
 * still only reflects a peer's START -- so the merged OVMX<->VAX path is
 * BYTE-IDENTICAL (the VAX drives the START; OVMX must not also drive it). The
 * FSM's Figure 2-7 collision handling means that even if BOTH OVMX nodes
 * initiate within the same second, each resolves the other's START cleanly
 * (START SENT -> START RECEIVED -> OPEN), so a single initiator is sufficient
 * and a symmetric pair is safe.
 *
 * ONE-SHOT, self-limiting: after the emit the circuit leaves CLOSED, so the
 * CLOSED guard blocks any re-initiate; the existing scsd_vc_fsm_timeout path
 * (SCS_VC_FORMATION_TIMEOUT_MS) owns retransmits from START SENT, and an
 * abandoned circuit is blocked by fsm.abandoned.
 */
static void scsd_start_initiate_for_peer(struct scsd_rx *rx, struct peer_state *ps,
                                         uint64_t now_ms)
{
    if (rx == NULL || ps == NULL || ps->pb == NULL || rx->vc_ctx == NULL) {
        return;
    }
    if (!rx->do_connect || !scsd_member_initiate_enabled()) {
        return;
    }
    /* Only once the channel is verified (rung-0), and only from a fresh CLOSED
     * circuit we have neither started nor completed and have not been asked to
     * reflect a peer's START into. */
    if (!ps->channel_up || ps->start_initiated || ps->start_replied ||
        ps->start_acked || ps->pb->fsm.abandoned ||
        ps->pb->vc_state != SCS_VC_CLOSED) {
        return;
    }
    if (!ps->vc.initialized) {
        scs_vc_init(&ps->vc);   /* send_seq=1: the grounded joiner value */
    }
    if (scs_vc_fsm_send_start(ps->pb, now_ms) == SCS_VC_ACT_SEND_START &&
        scsd_vc_emit(rx->vc_ctx, ps, SCS_VC_ACT_SEND_START)) {
        rx->start_sent++;
        ps->start_replies++;
        ps->start_initiated = 1;
        log_ts(stdout);
        printf(" SCSD-I-STARTTX, INITIATED round-0 START (member role,"
               " OVMX_MCAST_SOLICIT; VC %s) to"
               " %02x:%02x:%02x:%02x:%02x:%02x (sysid=%u node='%s' send_seq=%u"
               " incarnation=%u sys_incarnation=0x%016llx)\n",
               scs_vc_state_name(ps->pb->vc_state),
               ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
               ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
               rx->vc_ctx->scssystemid, rx->vc_ctx->node_name,
               ps->vc.seq.send_seq, ps->incarnation ? ps->incarnation : 1,
               (unsigned long long)ovmx_incarnation_time());
        fflush(stdout);
    }
}

/*
 * scsd_join_retx_tick - vms-694: drive scsd_join_retx_for_peer() from the main
 * loop's OWN clock, for every peer with an outstanding join step, so a
 * stalled step (JS_MSCP_CONNECT in particular) is retried even when the
 * peer's directed HELLO to OVMX pauses or reverts to plain/undirected framing.
 * See scsd_join_retx_for_peer()'s header for the lab evidence this closes.
 *
 * ADDITIVE, NOT A REPLACEMENT: scsd_handle_frame()'s directed-HELLO branch
 * keeps calling the SAME per-peer helper unchanged, so a peer that keeps
 * directing HELLO at OVMX sees no behaviour change from this. Both call sites
 * gate on the same ps->js_last_tx / ps->last_joiner_req timestamps, so this
 * tick cannot double-send within one JOIN_RETX_TIMEOUT_MS window of a
 * HELLO-triggered send, or vice versa.
 *
 * vms-d60: this tick ALSO drives scsd_start_initiate_for_peer() -- the member-
 * role round-0 START initiate -- for the same reason it drives the join retx:
 * it is a timer-driven member action ("a timer, not an event hook"), not a
 * response to an inbound frame. Both are no-ops with their kill-switch absent.
 *
 * OVMX_NO_JOIN_RETX_TICK=1 disables this call (guardrail 23: prove the gated
 * behaviour is suppressed) so the pre-fix HELLO-only path can be reproduced
 * in isolation to confirm this tick is what closes the stall.
 */
static void scsd_join_retx_tick(struct scsd_rx *rx, uint64_t now_ms)
{
    if (rx == NULL || !rx->do_connect || getenv("OVMX_NO_JOIN_RETX_TICK") != NULL) {
        return;
    }
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        struct peer_state *ps = &rx->peers[i];
        if (ps->pb == NULL) {
            continue;
        }
        scsd_start_initiate_for_peer(rx, ps, now_ms);
        scsd_join_retx_for_peer(rx, ps, (long)now_ms);
        /* vms-164d (DLM rung-1b): once the join to this peer is complete, fire
         * the one-shot cross-node $ENQ if OVMX_DLM_ENQ names a resource. A no-op
         * with the lab switch or the member-role flag absent. */
        scsd_dlm_send_enq(rx, ps);
        /* vms-eeb (DLM rung H9): node B (the master) SEEDS RRD's resource value
         * block once, at/after join, BEFORE node A's cross-node $ENQ can arrive.
         * A no-op on node A (OVMX_DLM_ENQ set) and without OVMX_DLM_H9. */
        scsd_dlm_h9_seed(rx, ps);
        /* rd vms-ec75 (DLM rung H11): a contender's timed hold-then-wait sequence
         * to the master C -- establishes its end of the cross-node deadlock cycle.
         * A no-op on the master node and without OVMX_DLM_EC75. */
        scsd_dlm_ec75_drive(rx, ps, now_ms);
    }
    /* vms-2bf (DLM rung H10a): node A latches the tracked resource's directory
     * CSID over the FULL membership, once, before any departure. Not per-peer --
     * a local read of our own executive; self-latches and is a no-op unless
     * OVMX_DLM_H10_RES names a resource (the readback node). */
    scsd_dlm_h10_before_read();
}

/*
 * scsd_recnx_tick - vms-c7d: drive each peer's CSB RECNXINTERVAL reconnect loop
 * from the main loop's own clock (transcript p. 7-30). For every peer whose CSB
 * is in WAIT/RECONNECT after a non-last-gasp VC break, this fires the
 * once-a-second reconnect beat and, when the max(RECNXINTERVAL, remote) period
 * expires with no peer-started transition, proposes a cluster state transition.
 * A peer whose circuit is OPEN (the normal case) is inert here.
 *
 * WHAT THIS SHIPS AT 0.3, AND WHAT IT DOES NOT. The state machine, the cadence
 * and the membership-hold are LIVE (and the unit test proves them on an injected
 * clock). The reconnect ATTEMPT and the transition PROPOSAL are logged here but
 * the actual emission of reconnect frames onto the wire, and the live
 * observation that a real peer restores the VC within the interval, are the
 * admission-gated 0.4 LIVE BRACKET -- they need a real peer and a real VC
 * breakage, which CI (no /dev/vms, no lab) cannot supply. OVMX therefore does
 * NOT invent new reconnect wire bytes here; a re-formation reuses the existing,
 * validated scs_vc formation path when a directed HELLO next arrives.
 *
 * OVMX_NO_RECNX_TICK=1 disables this loop driver (guardrail 23: prove the gated
 * behaviour is suppressed). OVMX_NO_RECNX_RECONNECT=1 is the deeper switch: it
 * stops any peer ever entering WAIT, so this tick has nothing to act on.
 */
/*
 * scsd_publish_membership - vms-8d4: drive the executive's cluster-membership
 * block (VMS_IOCTL_CLUSTER_MEMBER_SET/CLEAR) from the connection manager's LIVE
 * member set, so DCL SHOW CLUSTER and F$GETSYI (both separate processes, both
 * reading /dev/vms) see reality instead of a hardcoded NOTMEMBER. vms-551 (then
 * vms-5919) cut both readers over to the executive; vms-967d retired the
 * earlier userspace file bridge this function used to also publish to -- see
 * scs_membership.h for what remains of that contract (struct-only now).
 *
 * WHAT COUNTS AS A MEMBER. A peer is a member iff its VMS$VAXcluster connection
 * has reached OPEN (ps->vaxcluster_open_reached, the "honest membership fact"
 * latch documented on the struct field) OR the coordinator has published its
 * post-commit membership burst for us (ps->membership_bursts > 0, the AUTHENTIC
 * "OVMX is now a committed cluster member" signal per its struct field note).
 * Those are the two signals the code already trusts to mean "OVMX is admitted".
 * When at least one peer qualifies we are in a cluster, so the LOCAL node is a
 * member too and leads the list. When NONE qualify we are not (yet) a member:
 * CLEAR every executive entry this function last SET, so SHOW CLUSTER honestly
 * reports NOTMEMBER.
 *
 * Throttled to ~1 Hz off the main loop's clock -- membership changes are rare
 * and a reader only needs current-within-a-second state.
 */
static void scsd_publish_membership(struct scsd_rx *rx, uint64_t now_ms)
{
    if (rx == NULL || rx->peers == NULL || rx->vc_ctx == NULL) {
        return;
    }
    static uint64_t last_publish_ms = 0;
    if (last_publish_ms != 0 && now_ms - last_publish_ms < 1000) {
        return;
    }
    last_publish_ms = now_ms;

    struct scs_cluster_view view;
    memset(&view, 0, sizeof(view));

    /* Slot 0 is reserved for the local node; filled in only if we have peers. */
    int peer_members = 0;
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        struct peer_state *ps = &rx->peers[i];
        if (ps->pb == NULL) {
            continue;
        }
        if (!ps->vaxcluster_open_reached && ps->membership_bursts <= 0) {
            continue;
        }
        if (view.n_members + 1 >= SCS_MEMBERSHIP_MAX_NODES) {
            break;
        }
        /* +1: leave members[0] for the local node, added below. */
        struct scs_cluster_member *m = &view.members[1 + peer_members];
        m->sysid = peer_node_number(ps);
        /* Peer SCSNODE name is not reliably learned per-peer; the renderer
         * falls back to the SCSSYSTEMID when node is empty. */
        m->node[0] = '\0';
        strncpy(m->state, "MEMBER", sizeof(m->state) - 1);
        peer_members++;
    }

    /*
     * Cluster membership crosses into the executive (rd vms-551,
     * docs/design-cluster-membership-executive.md). This function drives the
     * executive's membership block via SET/CLEAR (a LOCAL ioctl on our own
     * /dev/vms -- no new SCS wire send, so no CHOKED send-site table entry).
     * vms-967d retired the userspace file bridge this function used to also
     * publish to; both readers (SHOW CLUSTER, F$GETSYI) read the executive
     * block exclusively now (vms-551, vms-5919).
     *
     * CSID convention: this node's own identity convention already treats
     * the SCSSYSTEMID-derived low-16 wire address as the DLM CSID (see
     * scsd_dlm_h10_depart_ingress above, departed = peer_node_number(ps)).
     * The same value is exactly what this loop already stores in
     * m->sysid for both the local node (rx->vc_ctx->scssystemid) and every
     * peer (peer_node_number(ps)), so csid == sysid here -- one identity,
     * not two independent ones to keep synchronized.
     *
     * scsd_membership_last_csids/_last_n remember what THIS function last
     * SET, purely to detect a departure (a csid that was SET last cycle and
     * is no longer live) and CLEAR exactly that -- CLEAR is idempotent, so
     * this diffing is an efficiency choice, not a correctness requirement.
     */
    static uint32_t scsd_membership_last_csids[SCS_MEMBERSHIP_MAX_NODES];
    static int scsd_membership_last_n;

    if (peer_members == 0) {
        /* Not a member of any cluster right now -> NOTMEMBER is the truth. */
        for (int i = 0; i < scsd_membership_last_n; i++) {
            (void)scsd_cluster_member_clear_ioctl(scsd_membership_last_csids[i]);
        }
        scsd_membership_last_n = 0;
        return;
    }

    /* We are a member: lead with the local node, then the peers gathered above. */
    struct scs_cluster_member *local = &view.members[0];
    if (rx->vc_ctx->node_name != NULL) {
        strncpy(local->node, rx->vc_ctx->node_name, sizeof(local->node) - 1);
    }
    local->node[sizeof(local->node) - 1] = '\0';
    local->sysid = rx->vc_ctx->scssystemid;
    strncpy(local->state, "MEMBER", sizeof(local->state) - 1);
    view.n_members = 1 + peer_members;

    /* Drive the executive: SET every live member this cycle, then CLEAR any
     * csid that was live last cycle and is not live this cycle -- a real
     * departure, never a fabricated one (INV-6). */
    uint32_t cur_csids[SCS_MEMBERSHIP_MAX_NODES];
    int n_cur = 0;
    for (int i = 0; i < view.n_members && i < SCS_MEMBERSHIP_MAX_NODES; i++) {
        struct scs_cluster_member *m = &view.members[i];
        (void)scsd_cluster_member_set_ioctl(m->sysid, m->sysid, m->node, m->state);
        cur_csids[n_cur++] = m->sysid;
    }
    for (int i = 0; i < scsd_membership_last_n; i++) {
        uint32_t old_csid = scsd_membership_last_csids[i];
        int still_present = 0;
        for (int j = 0; j < n_cur; j++) {
            if (cur_csids[j] == old_csid) {
                still_present = 1;
                break;
            }
        }
        if (!still_present) {
            (void)scsd_cluster_member_clear_ioctl(old_csid);
        }
    }
    memcpy(scsd_membership_last_csids, cur_csids, sizeof(uint32_t) * (size_t)n_cur);
    scsd_membership_last_n = n_cur;
}

static void scsd_recnx_tick(struct scsd_rx *rx, uint64_t now_ms)
{
    if (rx == NULL || getenv("OVMX_NO_RECNX_TICK") != NULL) {
        return;
    }
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        struct peer_state *ps = &rx->peers[i];
        if (ps->pb == NULL) {
            continue;
        }
        if (ps->csb.state != SCS_CSB_WAIT && ps->csb.state != SCS_CSB_RECONNECT) {
            continue;
        }
        enum scs_recnx_action a =
            scs_csb_reconnect_tick(&ps->csb, now_ms, /*peer_transition_started=*/0);
        if (a == SCS_RECNX_ACT_RECONNECT) {
            log_ts(stdout);
            printf(" SCSD-I-RECNXTRY, reconnect attempt %u to"
                   " %02x:%02x:%02x:%02x:%02x:%02x (CSB RECONNECT, membership"
                   " still HELD) (p. 7-30)\n",
                   ps->csb.attempts,
                   ps->pb->remote_port_addr[0], ps->pb->remote_port_addr[1],
                   ps->pb->remote_port_addr[2], ps->pb->remote_port_addr[3],
                   ps->pb->remote_port_addr[4], ps->pb->remote_port_addr[5]);
            fflush(stdout);
        } else if (a == SCS_RECNX_ACT_PROPOSE_TRANSITION) {
            log_ts(stdout);
            printf(" SCSD-I-RECNXEXPIRED, RECNXINTERVAL period elapsed with no"
                   " reconnect to %02x:%02x:%02x:%02x:%02x:%02x -- proposing a"
                   " cluster state transition to remove it (p. 7-30)\n",
                   ps->pb->remote_port_addr[0], ps->pb->remote_port_addr[1],
                   ps->pb->remote_port_addr[2], ps->pb->remote_port_addr[3],
                   ps->pb->remote_port_addr[4], ps->pb->remote_port_addr[5]);
            fflush(stdout);
        }
    }
}

/*
 * scsd_diskrun_gate_ms - how long the pure-server disk-discovery run waits
 * before it opens OUR SCS$DIRECTORY client connection anyway.
 *
 * Default 2000 ms sits inside the 1.4-4.4 s gap a real joiner leaves between
 * its op 0x01 and its op 0x02 while it runs its own disk-client discovery
 * (spec sec 4c.8) -- that is an AUTHENTICITY placement, replaying where a real
 * joiner's own run falls, not a join-success requirement. CORRECTED
 * (vms-5c7e): "the run must happen inside that window or the join suffers"
 * is NOT a claim this lab's evidence supports -- spec sec 4(O.4)'s `E8`
 * control arm ran with the run suppressed entirely (OVMX_NO_DISKRUN_UNGATE=1,
 * zero PS SCS$DIRECTORY CONNECT_REQ on the wire) and still reached
 * CLUSTER_NODES=3 on the identical schedule as the arms that ran it. Do not
 * reintroduce the timing-gates-admission claim; the authenticity rationale
 * above is the only one this lab has grounded. OVMX_DISKRUN_GATE_MS
 * overrides the default; a value of 0 or an unparseable one leaves the
 * default, which is why the env read is a function rather than an inline
 * strtoul -- vms-ebb's bracket has to be able to state what the gate was
 * without re-reading main().
 */
#define SCSD_DISKRUN_GATE_MS_DEFAULT 2000UL

static unsigned long scsd_diskrun_gate_ms(void)
{
    const char *e = getenv("OVMX_DISKRUN_GATE_MS");
    if (e != NULL && *e != '\0') {
        unsigned long v = strtoul(e, NULL, 10);
        if (v > 0UL) {
            return v;
        }
    }
    return SCSD_DISKRUN_GATE_MS_DEFAULT;
}

/*
 * scsd_diskrun_ungate_tick - DISK DISCOVERY HAS EXACTLY ONE TRIGGER, AND THIS
 * IS IT. Returns the number of runs started on this tick.
 *
 * ORIGINALLY (vms-2f3 step 4) this was the SECOND of two entry points into the
 * pure-server disk-CLIENT machine (OUR SCS$DIRECTORY connect -> MSCP$DISK
 * lookup -> SCC/GUS walk). The first fired immediately off an inbound op 6 on
 * SCS_DIR_OVMX_CONID; this one existed because on a REJOIN that op 6 never
 * arrives -- 0 of 3 peers send it (spec/handoff sec 4d.5) -- so a returning
 * identity never ran the discovery that sec 4c.8 shows every real joiner
 * performs in the 1.4-4.4 s between its op 0x01 and its op 0x02.
 *
 * CORRECTED (vms-096). The first trigger is GONE, and the paragraph that used
 * to stand here was doubly stale:
 *   - "started in exactly two places" -- there is ONE. The op-6 handler that
 *     set psc_credit_done sat INSIDE an `if (cm_op == 8)` branch and was
 *     unreachable, so the flag was permanently 0 and the immediate trigger it
 *     gated could never fire. Both are deleted.
 *   - "PSCLIENT fires 33 times on a join and 0 times on a rejoin" was MEASURED
 *     ON worktree-760-active-directory, where the op-6 handler really was
 *     reachable. It does not describe this branch and is not restated as if it
 *     did.
 *
 * ===== RULED (vms-ebb, 2026-08-05): ONE TRIGGER STAYS, AND THE REASON IS A
 * BRACKET, NOT THE ABSENCE OF A SIGNAL. READ THE SECOND BULLET. =====
 *
 * vms-096 left this open because the only passing acceptance bracket (spec sec
 * 4(O.1), runs B1/B3/B2) ran the DEFAULT environment -- no OVMX_PURE_SERVER --
 * so it never entered this block at all and could not speak to it either way.
 * vms-ebb ran the missing bracket: three pure-server arms on ONE lab-2 pod
 * (`vaxlab-1`), control BETWEEN the two test arms, identity read off the
 * capture, one binary md5-verified in-pod before and after every arm. Spec sec
 * 4(O.4) carries the figures. What it measured:
 *
 *   - THIS GATE IS WHAT STARTS THE RUN. PSC-UNGATED went 2 -> 0 -> 2 across
 *     test/control/test, and the PS SCS$DIRECTORY CONNECT_REQ (local Con.ID
 *     slot 0x000C) is on the wire in both test arms and ABSENT from the
 *     control. The kill switch was RUN, not asserted (guardrail 23).
 *
 *   - THE IMMEDIATE TRIGGER IS NOT DEAD FOR WANT OF A SIGNAL, and any claim
 *     that it is would be false. The peer initiates a p. 2-26 symmetric
 *     teardown of OUR SCS$DIRECTORY server connection -- peer DISCONNECT_REQ,
 *     our DISCONNECT_RSP, our own DISCONNECT_REQ, peer DISCONNECT_RSP, inside
 *     400 us -- TWICE per run (once per VAX) in 3 of 3 arms, at t+0.9/1.4,
 *     t+3.8/4.3 and t+2.3/3.8. That is exactly the frame the deleted trigger
 *     fired on, and the vms-591/vms-dd5 classifier now handles it
 *     (scsd_disconnect_dialogue). Re-attaching there would start the disk run
 *     2.1-2.9 s earlier than this gate does.
 *
 *   - AND IT BUYS NOTHING ON THIS PATH. All three arms reached CLUSTER_NODES=3
 *     at t+13 s -- INCLUDING the control, in which disk discovery never ran at
 *     all. Admission on this lab does not wait on the disk run, so 2.1-2.9 s of
 *     earlier start has no measured effect to be an improvement to, while a
 *     second entry point restores exactly the two-writer shape vms-096 deleted.
 *
 * So: not re-attached. NOT because the signal is missing -- it is there, it is
 * timed, and sec 4(O.4) records where it would attach and what it would gain --
 * but because nothing this bracket can reach is waiting for it.
 *
 * WHAT THIS BRACKET DOES NOT SETTLE, and it is the case that motivated the
 * trigger: the REJOIN. All three arms are FIRST joins by identities that had
 * never been admitted anywhere. Sec 4e.3's peer-side evidence is about a
 * refused rejoin -- the peer holds VMS$DISK_CL_DRVR in `con_sent` with a zero
 * Remote Con. ID, where a successful join leaves MSCP$DISK `open` -- and
 * whether the op 6 above even arrives on a rejoin is `vms-449`'s bracket, not
 * this one. If it does and the earlier start matters there, this ruling is the
 * thing to re-open, with the attachment point already measured.
 *
 * WHY IT IS A FUNCTION (vms-ebb). It was an inline block in main()'s loop, and
 * SCSD_UNIT_TEST renames main() away -- so the ONE trigger the daemon has was
 * compiled but reachable from no test, exactly the gap vms-abc closed for the
 * retransmit tick and vms-fb1 closed for the receive path. Nothing about the
 * body changed in the move: same guards, same order, same log line.
 *
 * Kill-switch OVMX_NO_DISKRUN_UNGATE=1 restores the gated behaviour so this can
 * be refuted by a matched control in the same session (guardrail 21).
 */
static unsigned scsd_diskrun_ungate_tick(struct scsd_rx *rx, uint64_t now_ms)
{
    if (rx == NULL || !rx->do_connect) {
        return 0;
    }
    if (getenv("OVMX_PURE_SERVER") == NULL ||
        getenv("OVMX_NO_DISKRUN_UNGATE") != NULL) {
        return 0;
    }
    unsigned long gate_ms = scsd_diskrun_gate_ms();
    unsigned started = 0;
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        struct peer_state *ps = &rx->peers[i];
        if (!(ps->pb != NULL) || !ps->cm_config_sent || ps->psc_gate_ms == 0) {
            continue;
        }
        if (ps->psc_step != PSC_IDLE || ps->psc_dir_sent) {
            continue; /* the run is already going */
        }
        if (now_ms - ps->psc_gate_ms < (uint64_t)gate_ms) {
            continue;
        }
        if (ps_send_dir_connect(rx->sock, rx->ifindex, ps,
                                rx->our_hw_mac, rx->our_src_logical)) {
            ps->psc_step = PSC_DIR_CONNECT;
            ps->psc_retx = 0;
            rx->psc_ungated++;
            started++;
            log_ts(stdout);
            printf(" SCSD-I-PSCUNGATE, no op 6 on our server dir connection"
                   " after %lums -- opened OUR SCS$DIRECTORY client connect"
                   " local=0x%08X seq=%u (disk-discovery step 1, UNGATED)\n",
                   gate_ms, (unsigned)PS_PS_DIR_CONID(ps), ps->psc_dir_req_seq);
            fflush(stdout);
        }
    }
    return started;
}

/*
 * ============ vms-591: CLEAN SHUTDOWN -- OVMX STOPS VANISHING ==============
 *
 * Before this item the daemon's whole shutdown was `g_stop = 1` and then exit.
 * Every connection it held was left formed from the peer's point of view, and
 * the peer's CDT for it stayed pending until its own timers gave up. p. 2-26
 * requires a SYSAP that is done with a connection to invoke DISCONNECT; a
 * process that just exits has invoked nothing.
 *
 * WHAT THIS DOES, in order:
 *   1. For each peer, invoke DISCONNECT on every connection on its circuit
 *      (scs_svc_disconnect_all). Each sends a DISCONNECT_REQ with the matching
 *      flag CLEAR -- we are initiating, not answering -- and moves the
 *      connection OPEN -> DISC SENT.
 *   2. Keep receiving for a BOUNDED time so the peer's DISCONNECT_RSP (and its
 *      own matching DISCONNECT_REQ) can be answered by the normal receive
 *      path, which is what walks each connection the rest of the way to
 *      CLOSED. No special-case receive loop: the same scsd_handle_frame() runs.
 *   3. Stop when every connection is CLOSED, or when the deadline expires.
 *
 * ============================ THE TIMEOUT ==================================
 *
 * SCSD_SHUTDOWN_WAIT_MS = 500, and A SHUTDOWN THAT HANGS WAITING FOR A PEER IS
 * WORSE THAN ONE THAT DOES NOT. The bound is what makes this safe; the value is
 * measured rather than picked:
 *
 *   Over all 47 lab captures, every VMS-origin DISCONNECT_REQ is answered by a
 *   DISCONNECT_RSP -- none unanswered -- and not one answer takes 10 ms.
 *   Seconds; re-derive with tools/cluster/scs_disc_measure.py:
 *
 * DISC-CENSUS-LAT: requests=220 answered=220 unanswered=0
 * DISC-CENSUS-LAT: min=0.000006 p50=0.000286 p90=0.001041 p99=0.003459 max=0.006919
 * DISC-CENSUS-LAT: over10ms=0
 *
 * 500 ms is 72x the largest REQ->RSP latency ever observed on our wire, and it
 * is still a fifth of a HELLO period, so a peer that is alive at all has had
 * many chances to answer. EXPLICIT NON-CLAIM, in the same terms spec sec 4(M)
 * uses: 6.919 ms is the largest latency in our captures, not an upper bound. A
 * loaded node could exceed it. The choice rests on the margin -- and, more
 * importantly, on the fact that EXCEEDING IT COSTS NOTHING BUT A LOG LINE. If
 * the deadline expires with connections still open, they are REPORTED
 * (SCSD-W-DISCPEND, counted in disc_shutdown_pending and printed in the exit
 * summary) and the daemon exits anyway. It never blocks on a peer.
 *
 * AND ON THE LAB IT SOMETIMES EXPIRES, WHICH IS WHY IT IS BOUNDED. Those
 * latency figures are VAX-to-VAX. This paragraph used to assert:
 *
 * PPD-REFUTED-BEGIN
 *     "A real VAX ANSWERS NO DISCONNECT_REQ OVMX SENDS -- not in 500 ms, and
 *      not in the 20 s a capture ran past one. It logs %PEA0, Inappropriate SCA
 *      Control Message instead."
 * PPD-REFUTED-END
 *
 * BOTH HALVES ARE REFUTED and must not be written back:
 *
 *   - vms-096 re-measured the same vaxlab-4 captures and found 10 of 10
 *     answerable OVMX DISCONNECT_REQs answered by a VMS-origin msgtype 7, in
 *     0.073-0.807 ms. spec sec 5 carries the census.
 *   - vms-0fe re-ran the experiment on lab-2 (runs 0feA1/0feB1/0feA2, main
 *     daemon, fresh identities): 0feA1 sent 3 DISCONNECT_REQ and got 2
 *     DISCONNECT_RSP back, and the "Inappropriate SCA Control Message" line did
 *     not appear on ANY of the four runs. What appears is the ordinary
 *     "Port has Closed Virtual Circuit" -- and the matched control 0feB1 drew
 *     that line while sending ZERO disconnect frames, so it is not ours at all.
 *
 * So the wait CAN complete, and does. It is kept, and kept short, because it
 * still expires whenever a connection's peer has already torn the VC down (the
 * SCSD-W-DISCPEND path below) and the daemon must not block on a peer.
 *
 * OVMX_SHUTDOWN_WAIT_MS overrides the value; 0 disables the wait entirely (send
 * and go). OVMX_NO_CLEAN_SHUTDOWN=1 skips the whole function -- and, because
 * the kill switch is enforced inside scs_disc_build_*(), would also make every
 * frame here unbuildable even if this check were removed.
 */
#define SCSD_SHUTDOWN_WAIT_MS 500

static unsigned scsd_open_connection_count(void)
{
    unsigned n = 0;
    if (!scsd_cdl_ready) {
        return 0;
    }
    for (unsigned i = 0; i < SCS_CDL_ENTRIES; i++) {
        const struct scs_cdt *cdt = scsd_cdl.entry[i];
        if (cdt == NULL || !cdt->in_use || cdt->listening) {
            continue;
        }
        if (scs_conn_state_of(cdt) != SCS_CONN_CLOSED) {
            n++;
        }
    }
    return n;
}

static long scsd_shutdown_wait_ms(void)
{
    const char *e = getenv("OVMX_SHUTDOWN_WAIT_MS");
    if (e != NULL && e[0] != '\0') {
        char *end = NULL;
        long v = strtol(e, &end, 10);
        if (end != NULL && *end == '\0' && v >= 0) {
            return v;
        }
    }
    return SCSD_SHUTDOWN_WAIT_MS;
}

/*
 * scsd_emit_clean_departure - vms-ab1 (spec 4(O.29)): announce OVMX's OWN
 * departure with the class-0x04 op-0x0d self-departure CM open -- one to each
 * member OVMX holds an OPEN VMS$VAXcluster VC to -- BEFORE tearing the
 * connections down. This drives the coordinator to REMOVE OVMX's Cluster System
 * Block immediately instead of holding it in a reconnect/long_break WAIT for
 * RECNXINTERVAL (spec 4(O.28), Davis p. 7-29) -- the very state that, left
 * alive, a rejoin-form return would re-arm forever. Pre-vms-ab1 OVMX emitted
 * NONE of this: its departure was only the per-connection SCS DISCONNECT below,
 * so the coordinator saw a bare VC break and waited out the reconnect period.
 *
 * Gated on the clean-leave default; OVMX_NO_LASTGASP=1 suppresses just this
 * last-gasp (the SCS DISCONNECT teardown still runs), and OVMX_REJOIN_CLEANLEAVE=0
 * suppresses the whole vms-ab1 path (pre-fix behaviour). CHOKED: it rides
 * send_frame_vc() and is refused on a non-OPEN circuit, which is correct -- a
 * departure open we cannot deliver is one the peer learns from the VC break
 * anyway. Header-only frame from scs_member_build_depart() (no node-parameter
 * body -- that is the vms-760 crash class).
 */
static unsigned scsd_emit_clean_departure(struct scsd_rx *rx)
{
    /* ⚠ DEFAULT OFF — OPT-IN ONLY (OVMX_LASTGASP=1). vms-ab1 (spec 4(O.29)):
     * the LIVE bracket on a fresh vaxlab-1 proved this op-0x0d self-departure open
     * CRASHES the real VAX coordinator (VAX2 bugchecked and rebooted right after
     * receiving it; the Part-2-only clean shutdown with NO last-gasp never did).
     * It is the clean-room-warned crash class (scs_member.h): a class-0x04
     * transition-OPEN emitted OUT of its grounded 0x12->0x03->0x0d->0x0a departure
     * choreography drives the coordinator's CNXMAN into an inconsistent-state
     * bugcheck. The AUTHENTIC immediate-removal signal is the PORT-LEVEL last-gasp
     * datagram (VAXcluster Principles p. 7-29), whose byte form is NOT grounded in
     * OVMX and must be RE'd before it is emitted -- deferred. The op-0x0d emit is
     * kept, behind an explicit opt-in, ONLY as an RE probe; it is NEVER part of the
     * default clean-leave path. Removal on a clean depart is left to the
     * coordinator's own RECNXINTERVAL (proven to remove: SDA long_break,removed). */
    if (getenv("OVMX_LASTGASP") == NULL) {
        return 0;
    }
    unsigned sent = 0;
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        struct peer_state *ps = &rx->peers[i];
        if (ps->pb == NULL || !ps->vaxcluster_open_reached) {
            continue;
        }
        struct scs_member_params mp;
        memset(&mp, 0, sizeof(mp));
        memcpy(mp.dst_mac, ps_port_addr(ps), 6);
        memcpy(mp.src_mac, rx->our_hw_mac, 6);
        memcpy(mp.src_logical, rx->our_src_logical, 6);
        memcpy(mp.peer_logical, ps_sys_addr(ps), 6);
        mp.remote_conid = ps->cfg_remote_conid ? ps->cfg_remote_conid : ps->remote_conid;
        mp.local_conid  = ps->cfg_local_conid  ? ps->cfg_local_conid  : PS_LOCAL_CONID(ps);
        mp.incarnation  = ps->incarnation;
        mp.recv_ack     = ps->vc.seq.recv_seq;
        mp.send_seq     = scs_seq_advance(&ps->vc.seq);
        mp.sysap_send_msg = ps->sysap_send++;
        mp.sysap_ack_msg  = ps->sysap_recv;
        mp.txn          = ps->own_txn;
        mp.checksum     = ++ps->own_cksum;
        uint8_t frame[SCS_MEMBER_FRAME_LEN];
        if (scs_member_build_depart(&mp, ps->barrier_epoch, frame) == 0 &&
            send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                          "clean CM self-departure op 0x0d (class-0x04, spec 4(O.29))",
                          frame, sizeof(frame)) > 0) {
            scs_vc_record_sent(&ps->vc, mp.send_seq, monotonic_ms());
            sent++;
            log_ts(stdout);
            printf(" SCSD-I-CMDEPART, emitted clean CM self-departure (op 0x0d"
                   " class-0x04) to node %u local=0x%08X remote=0x%08X epoch=0x%08X"
                   " -- coordinator should REMOVE our CSB now, not hold it in"
                   " reconnect/long_break (spec 4(O.29), Davis p. 7-29)\n",
                   (unsigned)(peer_node_number(ps) & 0x03ff),
                   (unsigned)mp.local_conid, (unsigned)mp.remote_conid,
                   (unsigned)ps->barrier_epoch);
            fflush(stdout);
        }
    }
    return sent;
}

/*
 * scsd_emit_port_lastgasp - vms-708 (spec 4(O.30)): emit OVMX's AUTHENTIC
 * PORT-LEVEL clean-leave "last gasp" datagram. This is the RE'd
 * immediate-removal signal a REAL VAX puts on the wire on SHUTDOWN.COM
 * (VAXcluster Principles p. 7-29), captured VAX-vs-VAX off vaxlab-0
 * (d94-708-leave frame 4899 + d94-708-leave2 frame 5015, 2026-08-11): a single
 * final MULTICAST HELLO whose abs-30 word is 0xb1 (vs 0xa0 periodic) carrying
 * the cluster nonce (vs zero). It drives peers to remove the departing node
 * WITHOUT the RECNXINTERVAL reconnect wait (the wire showed the coordinator
 * stop addressing the leaver by +3.3s -- no ~16-20s directed-reconnect storm).
 *
 * It REPLACES the CM-layer op-0x0d self-departure (scsd_emit_clean_departure),
 * which a live bracket proved CRASHES the coordinator (spec 4(O.29)): that was
 * a class-0x04 SCS transition-OPEN emitted out of choreography. THIS is a plain
 * HELLO variant -- best-effort CHANNEL traffic BELOW the virtual circuit
 * (p. 2-33), carrying no SCS transition state, and byte-for-byte what the
 * reference node emitted. DEFAULT ON.
 *
 * Kill switches: OVMX_NO_PORT_LASTGASP=1 suppresses just this frame (the SCS
 * DISCONNECT teardown still runs); OVMX_REJOIN_CLEANLEAVE=0 restores the whole
 * pre-clean-leave path. Rides send_frame_channel() -- the NISCA HELLO exemption
 * (the last gasp is addressed to the cluster multicast, rides no VC), same as
 * the periodic beacon; registered in the SEND SITE TABLE and the send-site
 * census (CHANNEL_CALLERS).
 */
static unsigned scsd_emit_port_lastgasp(struct scsd_rx *rx)
{
    if (!ovmx_rejoin_cleanleave() || getenv("OVMX_NO_PORT_LASTGASP") != NULL) {
        return 0;
    }
    if (rx->hello_params == NULL || rx->lab_nonce == NULL) {
        return 0;
    }
    rx->hello_params->timer_tick = hello_timer_tick100(); /* live 100ns tick, as the beacon */
    uint8_t frame[SCS_HELLO_FRAME_LEN];
    if (scs_hello_build_lastgasp_frame(rx->hello_params, rx->lab_nonce, frame) != 0) {
        return 0;
    }
    ssize_t sent = send_frame_channel(rx->sock, rx->ifindex, rx->hello_params->dst_mac,
                                      frame, sizeof(frame));
    if (sent <= 0) {
        return 0;
    }
    log_ts(stdout);
    printf(" SCSD-I-LASTGASP, emitted PORT-LEVEL clean-leave last gasp to the"
           " cluster multicast (HELLO abs-30=0xb1 + cluster nonce, spec 4(O.30),"
           " Davis p.7-29) -- peers should REMOVE us immediately, no"
           " RECNXINTERVAL reconnect wait\n");
    fflush(stdout);
    return 1;
}

/*
 * ovmx_crash_lastgasp_enabled - vms-2f3 kill switch for the corpse last gasp.
 * DEFAULT ON. OVMX_CRASH_LASTGASP=0 restores the pre-fix behaviour (a crashed
 * predecessor's CSB is left for the peer to age out over ~RECNXINTERVAL), so the
 * whale stays reproducible and the change stays bisectable (guardrail 23). Read
 * fresh every call.
 */
static int ovmx_crash_lastgasp_enabled(void)
{
    const char *off = getenv("OVMX_CRASH_LASTGASP");
    if (off != NULL && off[0] == '0' && off[1] == '\0') {
        return 0;
    }
    return 1;
}

/* vms-2f3: how many times to (best-effort) repeat the corpse gasp against
 * multicast loss, and how long to then hold OVMX's own join drive so the peer
 * can sequence departure -> CSB reclaim before our fresh channel-verify
 * re-establishes same-MAC continuity. ~3.3 s is the grounded reclaim latency
 * (o35proof); OVMX_CRASH_SETTLE_MS overrides. */
#define OVMX_CRASH_LASTGASP_REPEAT 3
#define OVMX_CRASH_SETTLE_MS       3300u

/*
 * scsd_emit_crash_lastgasp - vms-2f3 (spec 4(O.30)): THE WHITE-WHALE FIX.
 *
 * ROOT CAUSE. On a CRASH (SIGKILL, no clean shutdown) followed by a fast return
 * within RECNXINTERVAL, OVMX never emitted the port-level last gasp that
 * announces the PRIOR incarnation's departure -- so the peer, reading same-MAC
 * channel continuity (the pod stays up, the CLOCK_MONOTONIC HELLO timer never
 * resets), holds the crashed VMS$VAXcluster CDT open in reconnect/long_break for
 * ~RECNXINTERVAL (~100 s) and refuses the return onto that still-open CSB. OVMX
 * already emits the correct signal on a CLEAN shutdown (scsd_emit_port_lastgasp
 * via scsd_shutdown_teardown), which is exactly why clean rejoin works.
 *
 * THE FIX. Emit the CRASHED predecessor's port-level last gasp on the RETURN, at
 * birth, before the fresh join. The peer identifies the departing node by
 * SCSSYSTEMID (stable across incarnations, carried in the HELLO src logical
 * aa:00:04:00:<LE16(sysid)>) and removes the CSB it holds for the crashed one,
 * so the return admits in seconds instead of ~100 s.
 *
 * CRASH DETECTION is the prior-admission sidecar's clean flag: written 0 at
 * admission (prior_admission_save), rewritten 1 only after a clean-leave last
 * gasp (prior_admission_mark_clean). valid && clean!=1 at boot == crashed last
 * run. A clean rejoin (clean==1) or a first-timer (!valid) emits nothing.
 *
 * SAFETY (critical). This uses the PORT-LEVEL last gasp (scsd_emit_port_lastgasp,
 * a plain best-effort channel HELLO below the VC, abs-30=0xb1 + cluster nonce) --
 * NOT the CM-layer op-0x0d self-departure (scsd_emit_clean_departure), which a
 * live bracket proved CRASHES the real coordinator (spec 4(O.29)) because it is a
 * class-0x04 SCS transition-open emitted out of choreography. Same frame the
 * reference VAX puts on the wire on SHUTDOWN.COM; carries no SCS transition
 * state. Reusing scsd_emit_port_lastgasp also keeps every transmit inside the
 * one send-site the census (test_scsd_send_sites.py) enforces.
 */
static unsigned scsd_emit_crash_lastgasp(struct scsd_rx *rx)
{
    if (!ovmx_crash_lastgasp_enabled()) {
        return 0;
    }
    /* Only a node ADMITTED before AND with no recorded clean exit has a corpse
     * the peer may still hold. Emitting otherwise would be a harmless no-op (the
     * peer has no matching CSB to remove), but we stay honest and quiet. */
    if (!ovmx_prior.valid || ovmx_prior.clean == 1) {
        return 0;
    }
    if (!rx->emit_hello || rx->hello_params == NULL || rx->lab_nonce == NULL) {
        return 0;
    }
    log_ts(stdout);
    printf(" SCSD-I-CRASHGASP, prior admission to cluster formed=0x%016llx was NOT"
           " cleanly shut down (sidecar clean=0) -- emitting the crashed"
           " incarnation's PORT-LEVEL last gasp at BIRTH so the peer drops the"
           " corpse CSB (keyed on SCSSYSTEMID, stable across incarnations) before"
           " our fresh join re-masks the channel (spec 4(O.30), vms-2f3)\n",
           (unsigned long long)ovmx_prior.formed);
    fflush(stdout);

    unsigned sent = 0;
    for (int i = 0; i < OVMX_CRASH_LASTGASP_REPEAT; i++) {
        sent += scsd_emit_port_lastgasp(rx);
        if (i + 1 < OVMX_CRASH_LASTGASP_REPEAT) {
            struct timespec gap = { 0, 150L * 1000L * 1000L }; /* 150 ms */
            nanosleep(&gap, NULL);
        }
    }
    if (sent == 0) {
        return 0; /* port_lastgasp suppressed (e.g. OVMX_REJOIN_CLEANLEAVE=0) */
    }

    unsigned settle_ms = OVMX_CRASH_SETTLE_MS;
    const char *s = getenv("OVMX_CRASH_SETTLE_MS");
    if (s != NULL && s[0] != '\0') {
        long v = strtol(s, NULL, 10);
        if (v >= 0 && v <= 60000) {
            settle_ms = (unsigned)v;
        }
    }
    if (settle_ms > 0) {
        log_ts(stdout);
        printf(" SCSD-I-CRASHGASP, holding our join drive %u ms for the peer to"
               " sequence departure -> CSB reclaim before our fresh join\n",
               settle_ms);
        fflush(stdout);
        struct timespec hold = { settle_ms / 1000,
                                 (long)(settle_ms % 1000) * 1000L * 1000L };
        nanosleep(&hold, NULL);
    }
    return sent;
}

static void scsd_shutdown_teardown(struct scsd_rx *rx, uint8_t *buf, size_t bufsz)
{
    struct scs_svc_port *port = scsd_svc();
    unsigned driven = 0;
    long wait_ms;
    uint64_t deadline;

    if (port == NULL || !rx->do_connect) {
        return;
    }
    /* vms-ab1 (spec 4(O.29)): the clean CM self-departure rides the OPEN VC and
     * MUST go out before the SCS DISCONNECT tears that VC down. */
    (void)scsd_emit_clean_departure(rx);
    if (!scs_disc_enabled()) {
        log_ts(stdout);
        printf(" SCSD-I-NOCLEANSHUT, OVMX_NO_CLEAN_SHUTDOWN=1 -- no DISCONNECT"
               " frame will be sent and no teardown wait will run (this is the"
               " pre-vms-591 behaviour: exit without disconnecting)\n");
        fflush(stdout);
        return;
    }

    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        struct peer_state *ps = &rx->peers[i];
        if (ps->pb == NULL) {
            continue;
        }
        struct scsd_disc_emit_ctx e;
        memset(&e, 0, sizeof(e));
        e.sock = rx->sock;
        e.ifindex = rx->ifindex;
        e.ps = ps;
        e.our_hw_mac = rx->our_hw_mac;
        e.our_src_logical = rx->our_src_logical;
        e.matching = 0; /* WE are initiating: [60:62] = 0 (scs_disc.h) */

        struct scs_svc_args a = scsd_disc_args(&e, SCS_REASON_SYSAP_SHUTDOWN);
        struct scsd_svc_mark m = scsd_svc_mark();
        unsigned k = scs_svc_disconnect_all(port, ps->pb, &a);
        scsd_svc_settle(m);
        if (k > 0) {
            driven += k;
            log_ts(stdout);
            printf(" SCSD-I-SHUTDISC, invoked DISCONNECT on %u connection(s) to"
                   " peer %02x:%02x:%02x:%02x:%02x:%02x (%d DISCONNECT_REQ sent,"
                   " reason %u=%s)\n",
                   k, ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                   ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
                   e.req_sent, (unsigned)SCS_REASON_SYSAP_SHUTDOWN,
                   scs_reason_name(SCS_REASON_SYSAP_SHUTDOWN));
            fflush(stdout);
        }
    }
    if (driven == 0) {
        /* No VC to disconnect, but still announce departure at the port level
         * (vms-708): the multicast last gasp rides no circuit. */
        (void)scsd_emit_port_lastgasp(rx);
        /* vms-2f3: a clean-leave gasp went out -- record it so the next boot
         * does NOT emit a corpse gasp for this (properly departed) incarnation. */
        prior_admission_mark_clean();
        return;
    }

    /* Wait, bounded, for the peer to complete the dialogue. The frames are
     * handled by the ordinary receive path -- scsd_disconnect_dialogue() is
     * reached from scsd_handle_frame() exactly as it is during a run. */
    wait_ms = scsd_shutdown_wait_ms();
    deadline = monotonic_ms() + (uint64_t)wait_ms;
    while (wait_ms > 0 && monotonic_ms() < deadline) {
        if (scsd_open_connection_count() == 0) {
            break;
        }
        ssize_t n = scs_datalink_recv(rx->sock, buf, bufsz);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue; /* the socket's SO_RCVTIMEO is the inner bound */
            }
            break;
        }
        scsd_handle_frame(rx, buf, n);
    }

    disc_shutdown_pending = scsd_open_connection_count();
    log_ts(stdout);
    if (disc_shutdown_pending == 0) {
        printf(" SCSD-I-SHUTDONE, every connection reached CLOSED before exit\n");
    } else {
        printf(" SCSD-W-DISCPEND, %lu connection(s) had NOT reached CLOSED when"
               " the %ld ms shutdown wait expired -- exiting anyway rather than"
               " blocking on a peer\n", disc_shutdown_pending, wait_ms);
    }
    fflush(stdout);

    /* vms-708 (spec 4(O.30)): the port-level clean-leave last gasp is OVMX's
     * TRUE final frame -- emitted AFTER the SCS DISCONNECT dialogue, exactly as
     * the reference VAX emits it after its own dismount/disconnect traffic. This
     * is the authentic immediate-removal signal that REPLACES the crashing
     * op-0x0d (spec 4(O.29)). */
    (void)scsd_emit_port_lastgasp(rx);
    /* vms-2f3: our departure has been cleanly announced -- stamp the sidecar
     * clean=1 so a fast restart does NOT emit a corpse gasp (the peer already
     * removed us at departure; clean rejoin already works). */
    prior_admission_mark_clean();
}

/*
 * vms-f61 (spec §4(O.21)): the READMISSION MAP verdict.
 *
 * Grounded against the SUCCESS oracle's per-member JOIN handshake
 * (docs/design-rejoin-cm-state-map.md §4.1, Davis pp. 7-37/7-39/7-41/7-42): a
 * member ADMITS a (re)joining node by opening its OWN VMS$VAXcluster connection
 * toward the joiner and driving op00 -> (joiner op02) -> op03 COMMIT on it, after
 * which the members reconcile membership among themselves (op0c). To OVMX this is
 * visible as cm_responses>0 on that member and the membership latch reaching OPEN.
 *
 * The rejoin frontier (§4(O.20), re-grounded by vms-f61) is that on a RETURNING
 * identity the members run ZERO of these per-member handshakes: cm_responses==0
 * with the VC/channel/directory all up. This verdict names that state per member
 * so the next isolation is one log line, not a pcap dig. It is a PURE READ of
 * already-tracked per-peer fields -- it changes nothing on the wire (guard 8).
 *
 * ============================ HONESTY CAVEAT (vms-c21) ============================
 * THESE VERDICTS ARE OVMX-SIDE OBSERVATIONS, NOT AUTHORITATIVE MEMBERSHIP VERDICTS.
 * OVMX CANNOT classify the coordinator's membership decision from its own side.
 * The AUTHORITATIVE membership signal is each MEMBER's F$GETSYI("CLUSTER_NODES")
 * / the member's CDT layer -- NOT any OVMX-derived verdict here.
 *
 * Three consecutive RE sessions were MISLED by the old, conclusion-laden verdict
 * text of this classifier, each refuted by the member-side / oracle read:
 *   - §4(O.25) "the coordinator never proposes"        -> FALSE (it DOES; §4(O.32)).
 *   - §4(O.31) "SYSAP never re-opened / NO-ENGAGE"      -> FALSE (the member connects
 *                                                          and OVMX answers to OPEN; §4(O.32)).
 *   - §4(O.32) "cat 0x01 op 0x04 role 0x50 = a refusal DECISION"
 *                                                       -> FALSE (§4(O.33): op 0x04 fires
 *                                                          in the SUCCESSFUL join too; it is a
 *                                                          normal per-member barrier message.
 *                                                          The real outcome is a residual-CSB
 *                                                          reclaim RACE, not a refusal).
 * The verdict enum values below are kept as OVMX-side field-pattern buckets, and
 * the readmit_verdict_name() strings now report ONLY the observed field pattern,
 * each explicitly marked NON-AUTHORITATIVE. The per-verdict §4(O.x) narratives in
 * the comments below are the historical RE trail: read them as SUPERSEDED by this
 * caveat and by §4(O.32)/(O.33), never as current membership conclusions.
 * =================================================================================
 */
enum readmit_verdict {
    READMIT_NO_CHANNEL,   /* never reached transport with this peer */
    READMIT_NO_ENGAGE,    /* VC/channel/dir up, but member sent ZERO CM responses AND
                           * the SYSAP connection never re-opened: the returning-identity
                           * non-admission (§4(O.21)) */
    READMIT_RECLAIMED_NOJOIN, /* vms-944 (§4(O.23)): the SYSAP connection re-opened
                           * (open latched) but the member ran ZERO CM JOIN handshakes.
                           * SDA on the member (bracket 944B2r, vaxlab-1) showed this
                           * shape is the member RECLAIMING the residual CSB -- old CSB
                           * deallocated, a NEW CSB built at a new address with OVMX's
                           * fresh incarnation READ into it (Davis p. 7-24 DEAD ->
                           * p. 7-25 fresh CSB) -- and re-opening SCS, yet NEVER running
                           * the CM JOIN transition. It is a non-admission, but a
                           * DIFFERENT one from NO-ENGAGE: the reclaim + incarnation path
                           * worked; the failure is downstream at the CM JOIN. */
    READMIT_JOIN_ABANDONED, /* vms-0425 (§4(O.24)): OVMX DROVE its op 0x02 join request to
                           * THIS peer (joiner_cfg2_sent) and the peer returned ZERO CM
                           * responses -- no op 0x03 COMMIT. The wire diff of a same-pod
                           * first-join (Rf) vs return (Rr) for one identity showed the
                           * coordinator RELAYS (op 0x12) + COMMITS (op 0x03) a FIRST join
                           * but, for a RETURNING identity whose CSB it has just reclaimed,
                           * receives the identical op 0x02 and starts NO ADD transition --
                           * no op 0x12 relay to the other member, no op 0x03 to OVMX (Davis
                           * p. 7-38: the coordinator IGNORES the join request when its
                           * admission tests are not satisfied, and "no state transition
                           * occurs"). This is SHARPER than RECLAIMED_NOJOIN: it is the
                           * verdict for the member OVMX actually SENT the join request to
                           * (the coordinator), naming that the frontier is the coordinator
                           * abandoning an op02-DRIVEN join -- not an OVMX op02 omission. */
    READMIT_ENGAGED_NC,   /* member sent CM responses but membership not latched */
    READMIT_ADMITTED,     /* member ran the JOIN handshake and OVMX latched membership */
};

/* vms-c21: this returns an OVMX-SIDE FIELD-PATTERN bucket, NOT an authoritative
 * membership verdict. The §4(O.x) narratives in the enum doc-comments above
 * (e.g. "the coordinator abandons/ignores the join") are the SUPERSEDED RE
 * trail -- refuted by §4(O.32)/(O.33); see the HONESTY CAVEAT above the enum.
 * The classification logic below is a pure read of already-tracked fields and
 * is UNCHANGED by vms-c21 (only the human-readable strings were made honest). */
static enum readmit_verdict readmit_verdict_of(const struct peer_state *ps)
{
    if (ps == NULL || ps->pb == NULL || !ps->channel_up) {
        return READMIT_NO_CHANNEL;
    }
    /* vms-4dd (spec §4(O.22)): ADMITTED requires BOTH the membership latch AND
     * a nonzero per-member CM response count. The latch (vaxcluster_open_reached)
     * fires when the joiner/member CDT reaches OPEN, but the live lab bracket
     * `4dBr` (a RETURNING identity on vaxlab-1) proved that a member's CDT can
     * reach OPEN off a RESIDUAL/partial reconnect while the member runs ZERO CM
     * JOIN handshakes (cm_responses==0, XITDONE=0, no CN_3). Reading the latch
     * alone then over-reports that member as ADMITTED when the readmission never
     * happened -- a per-member false positive that masks a RETURNING-IDENTITY
     * NON-ADMISSION. A member that reached OPEN but sent 0 CM responses did not
     * run the per-member JOIN handshake this run; classify it as NO-ENGAGE, the
     * honest returning-identity verdict (INV-6: never report success from
     * residual state). */
    if (ps->vaxcluster_open_reached && ps->cm_responses > 0) {
        return READMIT_ADMITTED;
    }
    if (ps->cm_responses > 0) {
        return READMIT_ENGAGED_NC;
    }
    /* vms-0425 (spec §4(O.24)): OVMX drove its op 0x02 join request to THIS peer
     * (the deferred config/topology went out -- joiner_cfg2_sent) and got ZERO CM
     * responses back. OVMX sends op 0x02 only to the coordinator it selects
     * (Davis p. 7-37/38 "highest protocol level"), so this is the coordinator, and
     * the wire diff (Rf op 0x02 -> op 0x12 relay + op 0x03 commit; Rr the SAME op
     * 0x02 -> no relay, no commit) proves the coordinator ABANDONS the driven join
     * for a returning identity. Name it distinctly from RECLAIMED_NOJOIN (a member
     * we only REACHED) and NO_ENGAGE: here OVMX did its part -- it drove the join
     * request -- and the coordinator did not commit. Checked before the open latch
     * because OVMX's own VMS$VAXcluster VC often ends CONNSTUCK (DISC SENT) on the
     * return, so the latch under-reports; the op02-was-driven fact does not.
     *
     * vms-9a7 (spec §4(O.25)) REFINES the "why", grounded on SDA parked on the
     * coordinator across a same-boot fresh->crash->op02-driven-return bracket: the
     * coordinator does NOT "ignore" a received request (Davis p. 7-38). Its CNXMAN
     * never PROPOSES the addition -- on the FRESH join its console prints
     * "received VAXcluster membership request" -> "proposing addition" -> "completing
     * ... state transition"; on the RETURN (op 0x02 provably on the wire) it prints
     * ONLY "lost connection" / "timed-out lost connection", never "received request",
     * never "proposing addition". Its per-node CSB for the returner reaches
     * 02040000 status_rcvd but stalls in State 03 reconnect, never selected/member,
     * while SHOW CONNECTIONS shows its VMS$VAXcluster reconnect to the returner
     * parked in con_sent (Remote Con.ID 0) -- the readmission connection never
     * settles OPEN. The frontier relocates one layer DOWN, from CM admission to the
     * SCS connection layer; the §4(O.24) remote_conid=0 is the fingerprint of that
     * never-opening reconnect, not a separate defect. Classifier unchanged. */
    if (ps->joiner_cfg2_sent) {
        return READMIT_JOIN_ABANDONED;
    }
    /* vms-944 (spec §4(O.23)): SYSAP re-opened (latch set) but ZERO CM responses.
     * §4(O.22) read this exact shape (bracket `4dBr`) as a "residual/stale
     * reached-OPEN" and folded it into NO-ENGAGE. The `944B2r` SDA bracket
     * (vaxlab-1, OVMX_INCARNATION_TIME pinned to 1-OCT-2026) refuted that reading:
     * the member DEALLOCATED the residual CSB and built a NEW one at a new address
     * with OVMX's fresh incarnation read into it, and re-opened SCS -- a genuine
     * reclaim, not a stale reconnect -- yet ran no CM JOIN. Name it honestly so the
     * relocated frontier (reclaim works, CM JOIN does not run) is not masked as a
     * plain NO-ENGAGE. Still a non-admission (never ADMITTED). */
    if (ps->vaxcluster_open_reached) {
        return READMIT_RECLAIMED_NOJOIN;
    }
    return READMIT_NO_ENGAGE;
}

static const char *readmit_verdict_name(enum readmit_verdict v)
{
    /* vms-c21: OBSERVATIONS ONLY. Each string reports the OVMX-side field
     * pattern and is explicitly NON-AUTHORITATIVE. The authoritative membership
     * signal is the member's F$GETSYI("CLUSTER_NODES") / CDT layer -- see the
     * HONESTY CAVEAT above the enum. No string here asserts a coordinator
     * decision (refusal, abandonment, "never proposes"): those were the false
     * conclusions §4(O.32)/(O.33) refuted. */
    switch (v) {
    case READMIT_NO_CHANNEL: return "NO-CHANNEL(OVMX-observed: channel never reached transport with this peer)";
    case READMIT_NO_ENGAGE:  return "OBS-0-CM-RESP(OVMX-observed only: cm_responses=0, membership latch not set, OVMX did not drive op 0x02 to this peer. NON-AUTHORITATIVE -- 0 CM responses on OVMX's side does NOT mean the member did not engage: on the authoritative oracle the member DOES open its connection and propose the addition (spec 4(O.32)). Authoritative signal: the member's F$GETSYI(\"CLUSTER_NODES\") / CDT layer.)";
    case READMIT_RECLAIMED_NOJOIN: return "OBS-LATCH-0-CM-RESP(OVMX-observed only: membership latch reached OPEN, cm_responses=0. NON-AUTHORITATIVE -- an OVMX-side field pattern, not a membership verdict (spec 4(O.33)). Authoritative signal: the member's F$GETSYI(\"CLUSTER_NODES\").)";
    case READMIT_JOIN_ABANDONED: return "OBS-OP02-DRIVEN-0-CM-RESP(OVMX-observed only: OVMX drove op 0x02 to this peer and counted cm_responses=0. NON-AUTHORITATIVE -- OVMX cannot classify the coordinator's membership decision from its own side. op 0x02 IS delivered under the recv_ack ceiling (spec 4(O.27), DELIVERED); the real outcome is a residual-CSB reclaim RACE (spec 4(O.33)), not a coordinator verdict readable here. Authoritative signal: the member's F$GETSYI(\"CLUSTER_NODES\") / CDT layer.)";
    case READMIT_ENGAGED_NC: return "OBS-CM-RESP-NO-LATCH(OVMX-observed only: cm_responses>0, membership latch not OPEN. NON-AUTHORITATIVE -- confirm via the member's F$GETSYI(\"CLUSTER_NODES\"), spec 4(O.33).)";
    case READMIT_ADMITTED:   return "ADMITTED(OVMX-observed positive: cm_responses>0 AND membership latch OPEN. NON-AUTHORITATIVE -- confirm via the member's F$GETSYI(\"CLUSTER_NODES\"), which is the oracle, spec 4(O.33).)";
    default:                 return "?";
    }
}

/*
 * scsd_exit_summary - the end-of-run report, including the vms-dd5 connection
 * state-machine accounting and the stuck-connection scan.
 */
static void scsd_exit_summary(struct scsd_rx *rx, FILE *out)
{
    log_ts(out);
    fprintf(out, " SCSD-I-SUMMARY, %ld total 0x6007 frames received on '%s'%s\n",
            rx->total_frames, rx->ifname,
            rx->emit_hello ? " (and HELLO beacon was active)" : "");
    fprintf(out, "  HELLO=%ld SCS-FIXED=%ld SOLICIT=%ld OTHER=%ld RUNT=%ld\n",
            rx->counts[SCS_CLASS_HELLO], rx->counts[SCS_CLASS_SCS_FIXED],
            rx->counts[SCS_CLASS_SOLICIT], rx->counts[SCS_CLASS_OTHER], rx->counts[SCS_CLASS_RUNT]);
    if (rx->emit_hello) {
        fprintf(out, "  HELLO-SENT=%ld\n", rx->hello_sent);
    }
    if (rx->respond) {
        fprintf(out, "  DIRECTED-HELLO-SENT=%ld START-SENT=%ld START-ACK-SENT=%ld"
                " CONNECT-REQ-SENT=%ld CONNECT-RESP-SENT=%ld CREDIT-SENT=%ld RETX-SENT=%ld\n",
                rx->directed_sent, rx->start_sent, rx->start_ack_sent, rx->connect_req_sent, rx->connect_resp_sent,
                rx->credit_sent, rx->retransmit_sent);
        fprintf(out, "  DIR-CONNECT-RESP-SENT=%ld DIR-LOOKUP-RESP-SENT=%ld\n",
                rx->dir_conn_resp_sent, rx->dir_lookup_sent);
        /* vms-dd5: "A state that is entered and never left is detectable." Every
         * connection that is not OPEN at process exit is named here with its
         * CONID, which is the failure shape docs/HANDOFF-vms-2f3.md sec
         * 4M.18/4M.28 records on the peer side and which OVMX previously could
         * not represent at all. */
        fprintf(out, "  CONN-FSM: transitions=%lu illegal-events=%lu"
                " actions-required-but-not-emitted=%lu\n",
                conn_transitions, conn_illegal_events, conn_unemitted_actions);
        /* vms-6b3: p. 2-26's reason code as received. If the kill switch is
         * set, say so on the same line -- a run log must never read as "no peer
         * gave a reason" when the truth is "decoding was off". */
        fprintf(out, "  CONN-REASON: rej/disconn frames decoded=%lu"
                " carrying a nonzero reason=%lu%s\n",
                conn_reason_seen, conn_reason_nonzero,
                scs_reason_enabled() ? "" : " (OVMX_NO_REASON_CODE set --"
                                            " DECODING WAS OFF)");
        /* vms-591: the Figure 2-16 teardown, both halves. Printed
         * unconditionally: the number that matters is req-sent, because before
         * this item it was structurally 0 -- OVMX only ever reacted. If the
         * kill switch is set, say so on the same line, so a run log can never
         * read as "the peer never disconnected" when the truth is "we could
         * not have answered". */
        fprintf(out, "  DISCONNECT: req-recv=%lu rsp-sent=%lu req-sent=%lu"
                " rsp-recv=%lu closed=%lu simultaneous=%lu"
                " still-open-at-exit=%lu%s\n",
                disc_req_recv, disc_rsp_sent, disc_req_sent, disc_rsp_recv,
                disc_closed, disc_simultaneous, disc_shutdown_pending,
                scs_disc_enabled() ? "" : " (OVMX_NO_CLEAN_SHUTDOWN set --"
                                          " NO DISCONNECT FRAME WAS SENT)");
        /* vms-abc: the p. 2-31 message guarantees. Printed unconditionally --
         * "0 gaps, 0 breaks" is the answer on a healthy run and it must be
         * VISIBLE, not inferred from the absence of a warning. If the kill
         * switch is set, say so on the same line: a run log must never read as
         * "no circuit needed breaking" when the truth is "breaking was off". */
        /* sends-refused is the OTHER half of the guarantee, and the half a log
         * scan would otherwise miss: the SCSD-E-NOVC line is latched to one per
         * circuit-break, so the only place the TOTAL number of frames p. 2-31
         * kept off the wire appears is here. */
        fprintf(out, "  VC-GUARANTEES: seq-gaps=%lu vc-breaks=%lu"
                " connections-broken=%lu sysap-vc-loss-notifications=%lu"
                " sends-refused=%lu%s\n",
                vc_seq_gaps, vc_breaks, vc_conns_broken,
                sysap_vc_loss_notifications, vc_sends_refused,
                scs_vc_break_enabled() ? "" : "  [" SCS_VC_NO_BREAK_ENV
                                              " SET -- circuits were NOT broken]");
        /* vms-7c0: the p. 2-29 delivery ledger. This is the line that says
         * whether the CDL actually routed anything, and it is deliberately
         * printed unconditionally: a run whose app-messages count is large and
         * whose delivered count is 0 has a CDL that is decorative, and that has
         * to be visible in the run log rather than inferable from silence.
         * unknown-mtype is the datagram watch -- see scs_rx.h. */
        fprintf(out, "  RX-CDL: app-messages=%lu delivered=%lu no-cdt=%lu"
                " src-mismatch=%lu no-input-routine=%lu unknown-mtype=%lu"
                " sysap-input-calls=%lu cm-messages=%lu\n",
                rx_app_messages, rx_delivered_message, rx_deliver_no_cdt,
                rx_deliver_src_mismatch, rx_deliver_no_routine, rx_unknown_mtype,
                sysap_msg_input_calls, sysap_cm_messages);
        /* vms-ec7: THE CONTROL HALF of the same dispatch. RX-CDL above counts
         * only MTYPE 10; without this line a run log said how much SYSAP data
         * arrived and nothing at all about the connection dialogue that carried
         * it, so "the peer sent us four REJECT_REQs" was visible only if one of
         * them happened to hit a logging branch. Per-MTYPE, because the
         * INTERESTING number is which kind: a run with disconnect=2 and
         * connect-rsp=0 is a different failure from the reverse.
         * nonconformant is the honesty counter -- frames the pre-vms-ec7
         * classifier would have read a message type out of and that carry no
         * SCS envelope at all (see the note at that classifier). */
        fprintf(out, "  RX-CONTROL: total=%lu", rx_control_messages);
        for (unsigned _mt = 0; _mt <= SCS_ENV_MTYPE_CONTROL_MAX; _mt++) {
            if (rx_control_by_mtype[_mt] != 0) {
                fprintf(out, " %s=%lu", scs_env_mtype_name(_mt),
                        rx_control_by_mtype[_mt]);
            }
        }
        fprintf(out, " non-envelope-frames-declined=%lu\n",
                rx_control_nonconformant);
        /* vms-aa1: the pp. 2-43..2-45 account, as this run actually moved it.
         * Printed unconditionally and next to RX-CDL for the same reason that
         * line is: a run with app-messages in the thousands and stamped=0 has
         * flow control that is decorative, and that has to be readable in the
         * log rather than inferred. starved is the p. 2-45 Credit Wait backlog
         * this daemon cannot yet express (see scsd_credit_stamp_outbound) --
         * it is a REAL number, not a placeholder, and a large one is a defect
         * report. */
        fprintf(out, "  CREDIT: stamped=%lu units-sent=%lu starved=%lu no-cdt=%lu"
                " banked=%lu units-recv=%lu grants=%lu grant-units=%lu"
                " buffers-released=%lu%s\n",
                credit_send_stamped, credit_send_units, credit_send_starved,
                credit_send_no_cdt, credit_recv_banked, credit_recv_units,
                credit_grants_recv, credit_grant_units, credit_buffers_released,
                scs_credit_enabled() ? ""
                                     : "  [OVMX_NO_CREDIT_ACCOUNTING SET --"
                                       " nothing was debited, stamped or banked]");
        (void)scs_conn_report_stuck(&scsd_cdl, out);
        /* vms-17f: the p. 2-20/2-21 open transitions this run took, and the
         * departure sweep's work. PB-OPEN-REFRESHED is the p. 2-21 Note firing
         * -- a node that was here, left, and came back. */
        fprintf(out, "  PB-OPEN: new-sb=%lu refreshed=%lu existing-sb=%lu errors=%lu"
                " abandoned-masquerade=%lu\n",
                pb_open_results[SCS_OPEN_NEW_SB],
                pb_open_results[SCS_OPEN_EXISTING_REFRESHED],
                pb_open_results[SCS_OPEN_EXISTING_SB], pb_open_errors,
                pb_open_masquerades);
        fprintf(out, "  PEER-DEPARTURES=%lu CONNECTIONS-LOST=%lu TEARDOWNS-REFUSED=%lu"
                " LISTEN-TIMEOUT-MS=%llu%s\n",
                peer_departures, depart_connections_lost, depart_refusals,
                (unsigned long long)scs_depart_listen_timeout_ms(),
                scs_depart_enabled() ? "" : " (OVMX_NO_PEER_DEPART: sweep DISABLED)");
        /* vms-b1d: the p. 2-42 datagram buffer account. A datagram discard is
         * silent ON THE WIRE by design; it must not also be invisible HERE, or
         * the facility is indistinguishable from one that does nothing (INV-6).
         * These lines print zeros today and that is the honest state: nothing
         * in this daemon routes a received datagram through
         * scs_dgram_cdl_deliver(), so no buffer has ever been taken. See the
         * reachability note in scs_dgram.h. */
        (void)scs_dgram_report(&scsd_cdl, out);
        fprintf(out, "  CM-CONFIG-FRAMES=%ld CM-RESPONSES-SENT=%ld PADDED-HELLO-SENT=%ld\n",
                rx->cm_config_frames, rx->cm_response_sent, rx->padded_sent);
        /* vms-578: the three counters worktree-760 printed from main(). They
         * moved into struct scsd_rx with the rest, so a test that drives
         * scsd_handle_frame() can read them.
         * vms-c21 (spec 4(O.33)): CORRECTED. vms-2f3 framed this count as "the
         * coordinator declined us". It is NOT: the cat 0x01 op 0x04 role 0x50
         * message it counts occurs on SUCCESSFUL joins too and is ABSENT from
         * the reproducible failure, so it is a per-member CM barrier count, not
         * a refusal signal. Report it as an observation; draw no membership
         * conclusion from it -- the member's CLUSTER_NODES is the oracle. */
        fprintf(out, "  CM-OP04-BARRIER-SEEN=%ld (cat 0x01 op 0x04 role 0x50;"
                     " NON-AUTHORITATIVE, occurs on successful joins too -- spec"
                     " 4(O.33))\n", rx->cm_abort_seen);
        fprintf(out, "  CREDIT-RETX-ANSWERED=%ld\n", rx->credit_retx_seen);
        fprintf(out, "  MSCP-SERVER-ACCEPTS-SENT=%ld\n", rx->mscp_srv_accepts);
        fprintf(out, "  PSC-UNGATED=%ld\n", rx->psc_ungated);
        /* vms-7fe: the SDIR queue as it actually behaved this run. The two
         * refusal counts are the OVMX-chosen reply codes; BUSY is 0 for the
         * reason scs_sdir.h DESIGN CHOICE 3 states -- test_scsd_wire.c measures
         * that over the daemon's own receive path, and printing it here is how
         * a LIVE run stays falsifiable as well. */
        {
            const struct scs_sdir_queue *q = scs_svc_sdir(scsd_svc());
            fprintf(out, "  SDIR listening=%u", scs_sdir_count(q));
            for (const struct scs_sdir *s = scs_sdir_first(q); s != NULL;
                 s = scs_sdir_next(s)) {
                fprintf(out, " [%s conid=0x%08X %s]", s->sysap, s->conid,
                        scs_sdir_state_name(s->state));
            }
            fprintf(out,
                    " scans=%lu hits=%lu delivered=%lu retransmits=%lu"
                    " connect_scans=%lu no-such-sysap-sent=%lu busy-sent=%lu"
                    " refusals-unsent=%lu enabled=%s\n",
                    q->scans, q->hits, q->delivered, q->retransmits,
                    sdir_connect_scans, sdir_no_such_sysap, sdir_busy_replies,
                    sdir_refusals_unsent, scs_sdir_enabled() ? "YES" : "no (OVMX_NO_SDIR)");
        }
        /*
         * vms-66f: the p. 2-50 poller, as numbers. `skipped` is the interesting
         * one on a 2-node join: once OVMX's connection manager has its
         * VMS$VAXcluster connection, p. 2-50 disables polling for that pair, so
         * a run with connects=0 and skipped>0 is the RULE working, not the
         * poller failing. Reading connects=0 alone would get that backwards.
         */
        fprintf(out,
                "  POLLER SCS$DIR_LOOKUP interval=%us state=%s cycles=%lu"
                " completed=%lu abandoned=%lu disc-closed=%lu disc-unclosed=%lu connect-refused=%lu"
                " last-connect-status=%s last-refused-node=%02x:%02x:%02x:%02x:%02x:%02x connects-sent=%ld disconnects-sent=%ld inquiries-sent=%ld answers=%ld yes=%lu no=%lu"
                " unknown=%lu unsolicited=%lu notified=%lu skipped=%lu"
                " forced-cdt-release=%lu enabled=%s\n",
                scs_poll_interval_sec(), scs_poll_state_name(scsd_poller.state),
                scsd_poller.cycles_started, scsd_poller.cycles_completed,
                scsd_poller.cycles_abandoned, scsd_poller.disconnects_closed,
                scsd_poller.disconnects_unclosed,
                scsd_poller.connect_refused,
                scs_svc_status_name(scsd_poller.last_connect_status),
                scsd_poller.last_refused_node[0], scsd_poller.last_refused_node[1],
                scsd_poller.last_refused_node[2], scsd_poller.last_refused_node[3],
                scsd_poller.last_refused_node[4], scsd_poller.last_refused_node[5],
                rx->poll_connect_sent, rx->poll_disconnect_sent,
                rx->poll_inquiry_sent, rx->poll_answers, scsd_poller.answers_yes,
                scsd_poller.answers_no, scsd_poller.answers_unknown,
                scsd_poller.answers_unsolicited, scsd_poller.notifications,
                scsd_poller.skipped_disabled, scsd_poller.descriptors_forced,
                scs_poll_enabled() ? "YES" : "no (ships OFF; OVMX_PROCESS_POLLER=1 opts in,"
                                     " OVMX_NO_PROCESS_POLLER=1 forces off)");
        for (int i = 0; i < OVMX_MAX_PEERS; i++) {
            if (rx->peers[i].pb == NULL) {
                continue;
            }
            const uint8_t *pa = rx->peers[i].pb->remote_port_addr;
            fprintf(out,
                    "  PEER %02x:%02x:%02x:%02x:%02x:%02x vc=%s channel=%s directed_replies=%ld"
                    " incarnation=%u start_replied=%d start_acked=%d dir_connected=%s"
                    " dir_lookups=%ld connect_sent=%d connected=%s"
                    " rx->credit_sent=%ld retx=%u remote_conid=0x%08X"
                    " cm_config=%s cm_responses=%ld sysap_send=%u sysap_recv=%u"
                    " padded_replies=%ld padded_init=%d peer_padded_sca=%u"
                    " vaxcluster_member=%s conn[dir=%s member=%s joiner=%s]\n",
                    pa[0], pa[1], pa[2], pa[3], pa[4], pa[5],
                    scs_vc_state_name(rx->peers[i].pb->vc_state),
                    rx->peers[i].channel_up ? "UP" : "down", rx->peers[i].directed_replies,
                    rx->peers[i].incarnation,
                    rx->peers[i].start_replied, rx->peers[i].start_acked,
                    rx->peers[i].dir_connected ? "YES" : "no", rx->peers[i].dir_lookups_answered,
                    rx->peers[i].connect_sent, rx->peers[i].connected ? "YES" : "no",
                    rx->peers[i].credit_sent, rx->peers[i].vc.retransmit_count,
                    rx->peers[i].remote_conid,
                    rx->peers[i].cm_config_sent ? "YES" : "no", rx->peers[i].cm_responses,
                    rx->peers[i].sysap_send, rx->peers[i].sysap_recv,
                    rx->peers[i].padded_replies, rx->peers[i].padded_initiated,
                    rx->peers[i].peer_padded_sca,
                    /* vms-694 (§4(O.7)): the honest membership fact, LATCHED when
                     * the VMS$VAXcluster SYSAP connection reached OPEN, so it does
                     * NOT read "no" merely because the CDT was gracefully closed
                     * at teardown. Read THIS, not `connected=` (which tracks the
                     * member-opened connection the add-member wire never creates)
                     * nor the post-teardown `conn[joiner=]` sample, to answer "is
                     * OVMX a connected cluster member of this peer". */
                    rx->peers[i].vaxcluster_open_reached
                        ? "CONNECTED(reached-OPEN)" : "no",
                    /* "untracked" is NOT a state: it means no CDT was ever bound
                     * for that connection (machine off, the connection not yet
                     * formed, or the CDL genuinely full). Con.IDs are per-peer
                     * now (vms-694), so it no longer means the slot belonged to
                     * another peer. Do not read it as CLOSED. */
                    rx->peers[i].cdt_dir ? scs_conn_state_name(scs_conn_state_of(rx->peers[i].cdt_dir))
                                     : "untracked",
                    rx->peers[i].cdt_member
                        ? scs_conn_state_name(scs_conn_state_of(rx->peers[i].cdt_member))
                        : "untracked",
                    rx->peers[i].cdt_joiner
                        ? scs_conn_state_name(scs_conn_state_of(rx->peers[i].cdt_joiner))
                        : "untracked");
        }

        /* vms-f61 (spec §4(O.21)): the READMISSION MAP. Log-only; silence with
         * OVMX_NO_READMITMAP=1. Grounded verdict per member (readmit_verdict_of),
         * plus the incarnation OVMX presented this run -- the fresh-per-boot
         * [66:74] quadword (ovmx_incarnation_time(), scs_start.c) that should make
         * a returning identity look NEW to each member (Davis p. 7-24 DEAD ->
         * p. 7-25 fresh CSB).
         *
         * vms-c21 (spec §4(O.32)/(O.33)): these per-peer buckets are OVMX-SIDE
         * OBSERVATIONS, NOT authoritative membership verdicts -- see the HONESTY
         * CAVEAT above the readmit_verdict enum. A run with admitted=0 here is
         * NOT proof of a coordinator refusal: the members DO connect and propose
         * (§4(O.32)) and the true outcome is a residual-CSB reclaim RACE, not a
         * refusal (§4(O.33)). The authoritative signal is each MEMBER's
         * F$GETSYI("CLUSTER_NODES") / CDT layer, not anything printed here. */
        if (getenv("OVMX_NO_READMITMAP") == NULL) {
            unsigned long long incn = (unsigned long long)ovmx_incarnation_time();
            int reached = 0, admitted = 0, engaged = 0, no_engage = 0, reclaimed_nojoin = 0;
            int join_abandoned = 0;
            fprintf(out,
                    "  READMITMAP-CAVEAT: verdicts below are OVMX-SIDE OBSERVATIONS"
                    " (NON-AUTHORITATIVE). The authoritative membership signal is"
                    " each MEMBER's F$GETSYI(\"CLUSTER_NODES\") / CDT layer, not any"
                    " OVMX-derived verdict (spec 4(O.32)/4(O.33)).\n");
            for (int i = 0; i < OVMX_MAX_PEERS; i++) {
                if (rx->peers[i].pb == NULL) {
                    continue;
                }
                const uint8_t *pa = rx->peers[i].pb->remote_port_addr;
                enum readmit_verdict v = readmit_verdict_of(&rx->peers[i]);
                if (v != READMIT_NO_CHANNEL) {
                    reached++;
                }
                if (v == READMIT_ADMITTED) {
                    admitted++;
                } else if (v == READMIT_ENGAGED_NC) {
                    engaged++;
                } else if (v == READMIT_RECLAIMED_NOJOIN) {
                    reclaimed_nojoin++;
                } else if (v == READMIT_JOIN_ABANDONED) {
                    join_abandoned++;
                } else if (v == READMIT_NO_ENGAGE) {
                    no_engage++;
                }
                fprintf(out,
                        "  READMITMAP peer %02x:%02x:%02x:%02x:%02x:%02x"
                        " cm_responses=%ld membership_bursts=%ld member_vc=%s verdict=%s\n",
                        pa[0], pa[1], pa[2], pa[3], pa[4], pa[5],
                        rx->peers[i].cm_responses,
                        rx->peers[i].membership_bursts,
                        rx->peers[i].cdt_member
                            ? scs_conn_state_name(scs_conn_state_of(rx->peers[i].cdt_member))
                            : "untracked",
                        readmit_verdict_name(v));
            }
            /* vms-197 (spec §4(O.38)): the AUTHORITATIVE-ALIGNED cluster-admission
             * fact -- did OVMX receive the coordinator's op 0x06 MEMBERSHIP burst
             * this run? This is the ONE self-report field grounded 1:1 against the
             * member oracle (present on every WIN, absent on the LOSE); the per-peer
             * admitted/engaged buckets below remain OVMX-side field patterns
             * (§4(O.32)/(O.33) caveat). Report it explicitly so a reader does not
             * have to re-derive admission from the racy barrier or the heuristic
             * per-peer verdicts. */
            fprintf(out,
                    "  READMITMAP-SUMMARY incarnation_presented=0x%016llx%s"
                    " membership_committed=%s members_reached=%d admitted=%d engaged=%d"
                    " reclaimed_nojoin=%d join_abandoned=%d no_engage=%d --"
                    " %s (spec 4(O.32)/4(O.33)/4(O.38), docs/design-rejoin-cm-state-map.md)\n",
                    incn, incn ? "(live)" : "(frozen/template)",
                    ovmx_cluster.membership_committed
                        ? "YES(op 0x06 membership burst received -- AUTHENTIC admission, 1:1 with the member oracle, spec 4(O.38))"
                        : "no(no op 0x06 membership burst this run)",
                    reached, admitted, engaged, reclaimed_nojoin, join_abandoned, no_engage,
                    /* vms-c21: OBSERVATION ONLY. The counts above are OVMX-side
                     * field-pattern buckets; the line below states what OVMX saw,
                     * never a coordinator membership decision (it cannot know one
                     * from its own side, §4(O.32)/(O.33)). */
                    (reached > 0 && admitted == 0
                     && (no_engage + reclaimed_nojoin + join_abandoned) == reached)
                        ? "OVMX-SIDE OBSERVATION (NON-AUTHORITATIVE): every reached"
                          " member shows cm_responses=0 with no OVMX membership latch."
                          " This is NOT proof of a coordinator refusal or non-admission:"
                          " on the authoritative oracle the members DO open their"
                          " connections and propose the addition (spec 4(O.32)), and"
                          " where op 0x02 was driven it is delivered under the recv_ack"
                          " ceiling (spec 4(O.27)); the true outcome for a returning"
                          " identity is a residual-CSB reclaim RACE, not a refusal (spec"
                          " 4(O.33)). Read the member's F$GETSYI(\"CLUSTER_NODES\") to"
                          " know the actual membership."
                        : (admitted == reached && reached > 0)
                          ? "OVMX-SIDE OBSERVATION (NON-AUTHORITATIVE): all reached members"
                            " show cm_responses>0 and the OVMX membership latch OPEN --"
                            " an OVMX-side positive signal. Confirm via the member's"
                            " F$GETSYI(\"CLUSTER_NODES\"), spec 4(O.33)."
                          : "OVMX-SIDE OBSERVATION (NON-AUTHORITATIVE): mixed/partial --"
                            " see the per-peer OBSERVED buckets. Confirm actual membership"
                            " via the member's F$GETSYI(\"CLUSTER_NODES\"), spec 4(O.33).");
        }
    }
}


/*
 * resolve_cluster_iface - vms-5ad: resolve the interface SCSD binds when it
 * is started with NO wire-mode flags at all ("boot-cluster mode" -- see
 * main()'s comment below). OVMX RUN /DETACHED passes no argv
 * (dcl_cmd_process.c's qualifier table has no /PARAMETERS), so a booted SCSD
 * cannot be told --iface on the command line; it must self-resolve the same
 * NIC the executive's device table names ETH0:.
 *
 * MIRRORS exec_netdev_primary() (src/kernel-core/exec_kbackend.h sec 11,
 * lines 392-401): "find the host's PRIMARY (first, in kernel enumeration
 * order) non-loopback Ethernet net device." scs_datalink_primary_iface()
 * (scs_datalink.c) is the userspace twin of that exact rule -- if_nameindex +
 * SIOCGIFFLAGS/SIOCGIFHWADDR on Linux, getifaddrs+AF_LINK on NetBSD -- so
 * SCSD binds the SAME device the device table surfaced as ETH0:, without
 * asking the executive (SCSD is a userspace daemon; it has no /dev/vms
 * dependency of its own).
 *
 * Returns 0 and fills iface_out on success, -1 (errno set, ENODEV if no
 * interface qualifies) on failure -- the honest "no NIC" case. Never invents
 * an interface name (INV-6).
 */
static int resolve_cluster_iface(char *iface_out, size_t iface_out_len)
{
    return scs_datalink_primary_iface(iface_out, iface_out_len);
}

#ifdef SCSD_UNIT_TEST
/* vms-7be: tests/vmsscs/test_scsd_wire.c #includes this file and supplies its own
 * main(). The daemon entry point is RENAMED, not compiled out, so every static
 * helper above is compiled exactly as the daemon compiles it. */
int scsd_daemon_main(int argc, char **argv);
int scsd_daemon_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    const char *ifname = "br0";
    int duration = 0; /* 0 = run until SIGINT/SIGTERM */
    int emit_hello = 0;
    int respond = 0;      /* vms-5fe: reply to directed HELLOs (channel formation) */
    int do_connect = 0;   /* vms-5fe: also drive the VMS$VAXcluster SCS connect */
    int boot_cluster_mode = 0; /* vms-a84d: set only by argc==1 boot-cluster mode --
                                * a booted cluster participant must DRIVE the full
                                * NEW->MEMBER join sequencer (send the membership
                                * request), not merely beacon HELLOs. */
    int hello_interval = HELLO_DEFAULT_INTERVAL_SEC;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iface") == 0 && i + 1 < argc) {
            ifname = argv[++i];
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--emit-hello") == 0) {
            emit_hello = 1;
        } else if (strcmp(argv[i], "--respond") == 0) {
            respond = 1;
        } else if (strcmp(argv[i], "--connect") == 0) {
            do_connect = 1;
        } else if (strcmp(argv[i], "--hello-interval") == 0 && i + 1 < argc) {
            hello_interval = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--show-identity") == 0) {
            /* vms-9cf: report the cluster IDENTITY params this node would adopt
             * on boot -- SCSNODE, SCSSYSTEMID, ALLOCLASS -- read from the SYSGEN
             * store (OVMXVMSSYS.PAR; honors OVMX_SYSGEN_PATH), then exit. This
             * is the read-side adoption surface the R1 config-authoring proof
             * (vms-9cf) drives: SET the params the VMS way + WRITE CURRENT, then
             * a fresh SCSD picks them up here. It opens NO raw socket and needs
             * no privilege -- it is a pure read of the persisted param store, so
             * the store->boot->scsd round trip is genuine, not a per-process
             * fake (INV-6). It touches NO wire frame and does NOT read or emit
             * VOTES (the deliberate VOTES=0 non-voting join is untouched;
             * VOTES/quorum reconciliation is vms-41d, the cluster-wire session).
             *
             * A named store that carries SCSNODE but fails to yield it is fatal
             * here for the same reason it is fatal on a real boot
             * (resolve_node_identity): a wrong identity must never be invented. */
            char idnode[SYSGEN_STRVAL_LEN];
            if (resolve_node_identity(idnode, sizeof(idnode)) != 0) {
                return 1;
            }
            uint16_t idsysid = resolve_scssystemid();
            uint8_t  idalloc = resolve_alloclass();
            /* vms-c3b: RECNXINTERVAL is adopted the same read-side way, through
             * scsd_recnxinterval() -> sysgen_read_param() on the persisted
             * store. Reported here so the config-authoring proof can read back
             * the operator's authored interval from a fresh SCSD (INV-6: a real
             * store->boot->scsd round trip, no per-process fake). Surface only:
             * it does not touch the reconnect wire logic (vms-694). */
            unsigned idrecnx = scsd_recnxinterval();
            printf("SCSD-I-IDENT, SCSNODE=%s SCSSYSTEMID=%u ALLOCLASS=%u"
                   " RECNXINTERVAL=%u\n",
                   idnode, (unsigned)idsysid, (unsigned)idalloc, idrecnx);
            fflush(stdout);
            return 0;
        } else if (strcmp(argv[i], "--dlm-selftest") == 0) {
            /* vms-4b6 (DLM harness rung H0): drive ONE local cross-node DLM
             * dispatch through SCSD's OWN executive path --
             * scsd_dlm_dispatch_to_executive() -- and print the status the
             * executive returned, then exit. This is the single-node
             * foundation the two-node DLM harness (H1-H4) builds on: it proves
             * that the SCSD-carrying image COMPOSES WITH A REAL EXECUTIVE. On a
             * node with a real /dev/vms (the tests/qemu KTEST path, vms.ko
             * insmod'd) the rung-1 handler returns SS$_UNSUPPORTED (2296); with
             * NO executive (the Docker cluster harness -- Rule 9: Docker is not
             * a runtime, there is no /dev/vms) the SAME code path fails HONEST
             * with SS$_NOSUCHDEV (2680). It fabricates NOTHING (INV-6): it
             * prints verbatim whatever scsd_dlm_dispatch_to_executive() got.
             *
             * It builds a synthetic-but-well-formed BLKAST request (the same shape
             * scs_dlm_parse hands the input routines on a real received frame) so
             * the executive's VMS_IOCTL_DLM_XNODE handler is reached exactly as an
             * inbound cross-node message would reach it -- opens no socket, needs
             * no CAP_NET_RAW, touches no wire. The 2296-not-2680 flip is the
             * machine-checkable proof.
             *
             * BLKAST (not ENQ or DEQ) on purpose. The side-effect-free
             * "executive reached, honest status = 2296" probe has to name an op
             * the handler still declines with SS$_UNSUPPORTED and that touches no
             * lock state. ENQ now GRANTS a compatible cross-node lock (rung 2,
             * vms-e8f1) and DEQ now RELEASES/authorizes real lock state (rung 3,
             * vms-904c: a DEQ of an unknown handle returns SS$_IVLOCKID, not
             * SS$_UNSUPPORTED) -- both have moved off 2296. BLKAST as a RECEIVE op
             * is the holder-side blocking-AST delivery whose WIRE is deferred
             * (vms-6ca, rung H5): it still returns SS$_UNSUPPORTED and mutates
             * nothing -- the ideal probe H0 needs. */
            struct scs_dlm_msg m;
            memset(&m, 0, sizeof(m));
            m.op = SCS_DLM_OP_BLKAST;
            m.mode = LCK$K_NLMODE;
            m.req_lkid = 0x00040011u;
            m.master_lkid = 0x00080002u;
            m.req_csid = 1025u;
            m.master_csid = 1026u;

            uint32_t status = scsd_dlm_dispatch_to_executive(&m, NULL, NULL, NULL);
            printf("SCSD-I-DLMSELFTEST, executive DLM dispatch status=%u"
                   " (0x%08X)\n", (unsigned)status, (unsigned)status);
            fflush(stdout);
            /* Exit code carries the verdict too (the low byte of a VMS status
             * is not portable to $?, so map it): 0 iff the real executive was
             * reached (SS$_UNSUPPORTED, rung 1); nonzero otherwise -- including
             * the fail-honest SS$_NOSUCHDEV when no /dev/vms is present. The H0
             * driver asserts on the printed status line either way. */
            return (status == 2296u) ? 0 : 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                    "usage: %s [--iface IFACE] [--duration SECONDS] [--emit-hello]\n"
                    "          [--respond] [--connect] [--hello-interval SECONDS]\n"
                    "  SCS datalink listener: opens a raw-L2 datalink (AF_PACKET on\n"
                    "  Linux, bpf(4) on NetBSD) on ethertype 0x%04x (DEC SCA/LAVC),\n"
                    "  classifies received frames\n"
                    "  by the GROUNDED length rule, and logs them.\n"
                    "  --emit-hello        also multicast a spec-valid HELLO beacon\n"
                    "                      (identity from SCSNODE/cluster-group config;\n"
                    "                      see docs/cluster-protocol-spec.md sec 4a/4b)\n"
                    "  --respond           reply to a peer's DIRECTED HELLO with our own\n"
                    "                      directed HELLO to form the NISCA channel\n"
                    "                      (implies --emit-hello; spec sec 4b)\n"
                    "  --connect           additionally drive the VMS$VAXcluster SCS\n"
                    "                      connect: send CONNECT-REQUEST after the\n"
                    "                      channel forms and answer a peer CONNECT-\n"
                    "                      REQUEST with CONNECT-RESPONSE (spec sec 4g).\n"
                    "                      Implies --respond.\n"
                    "  --hello-interval N  seconds between HELLO beacons (default %d)\n"
                    "  --show-identity     print the cluster identity params this\n"
                    "                      node adopts from the SYSGEN store\n"
                    "                      (SCSNODE/SCSSYSTEMID/ALLOCLASS/\n"
                    "                      RECNXINTERVAL) and exit; opens no\n"
                    "                      socket (vms-9cf, vms-c3b)\n"
                    "  --dlm-selftest      drive ONE local cross-node DLM dispatch\n"
                    "                      through the executive (/dev/vms,\n"
                    "                      VMS_IOCTL_DLM_XNODE) and print the status;\n"
                    "                      SS$_UNSUPPORTED(2296) on a real executive,\n"
                    "                      SS$_NOSUCHDEV(2680) fail-honest without one.\n"
                    "                      Opens no socket (vms-4b6, DLM harness H0)\n"
                    "  (no arguments)      BOOT-CLUSTER MODE (vms-5ad): self-configure from\n"
                    "                      the SYSGEN store and run --connect persistently\n"
                    "                      on the primary Ethernet NIC iff VAXCLUSTER!=0;\n"
                    "                      exits 0 doing nothing on a standalone node\n"
                    "                      (VAXCLUSTER==0). This is what OVMX RUN /DETACHED\n"
                    "                      starts at boot, which can pass no argv.\n",
                    argv[0], SCA_ETHERTYPE, HELLO_DEFAULT_INTERVAL_SEC);
            return 0;
        }
    }

    /* vms-5ad: BOOT-CLUSTER MODE. When SCSD is invoked with NO wire-mode
     * flags at all (argc==1 -- exactly what OVMX RUN /DETACHED gives it: its
     * qualifier table has no /PARAMETERS, dcl_cmd_process.c), it is a
     * detached system process started from SCS_STARTUP.COM at boot, not the
     * lab probe (which always passes explicit flags -- --show-identity in
     * the bootable Dockerfile's self-check, --connect/--iface/etc. in the
     * lab harness). Every explicit-flag branch above is UNCHANGED; this path
     * is reached only when none of them fired.
     *
     * The node self-gates on VAXCLUSTER (VMS SYSGEN parameter semantics:
     * 0=disabled, 1=AUTO, 2=ALWAYS -- VSI OpenVMS System Management
     * Utilities Reference Manual; OVMX treats any nonzero value as
     * "participate," matching AUTO/ALWAYS both meaning the node forms or
     * joins a cluster). A standalone node (VAXCLUSTER==0, the shipped
     * OVMXVMSSYS.PAR default) prints an honest line and exits 0 -- SCSD
     * declining to run is not an error on a standalone system, and staying
     * silent instead would be the INV-6 fabrication (a "cluster service" that
     * quietly does nothing looks identical to one that never ran). Any
     * nonzero VAXCLUSTER self-configures and runs the --connect path
     * persistently (duration stays 0: run until SIGINT/SIGTERM, i.e. until
     * $ STOP or a reboot -- the same lifetime as any other detached system
     * process). */
    if (argc == 1) {
        uint32_t vaxcluster = 0;
        if (sysgen_read_param("VAXCLUSTER", &vaxcluster) != 0 || vaxcluster == 0) {
            log_ts(stdout);
            printf(" SCSD-I-STANDALONE, VAXCLUSTER=%u: node is standalone,"
                   " cluster connection manager not started\n",
                   (unsigned)vaxcluster);
            fflush(stdout);
            return 0;
        }

        /* vms-5ad: resolve the same NIC the executive's device table (vms_devtab.c)
         * names ETH0: -- see resolve_cluster_iface()'s header, above. Fail
         * honest (INV-6): a cluster-participating node with no Ethernet net
         * device to bind is a real misconfiguration, not something to paper
         * over with a fake or defaulted interface name. */
        static char boot_iface[IFNAMSIZ];
        if (resolve_cluster_iface(boot_iface, sizeof(boot_iface)) != 0) {
            fprintf(stderr,
                    "SCSD-E-NONIC, no Ethernet interface backs ETH0: (VAXCLUSTER=%u"
                    " names this node a cluster member, but no non-loopback"
                    " Ethernet net device was found to bind)\n",
                    (unsigned)vaxcluster);
            return 1;
        }
        ifname = boot_iface;
        do_connect = 1; /* implies respond -> emit_hello, below, exactly as
                          * an explicit --connect does */
        boot_cluster_mode = 1; /* vms-a84d: drive the NEW->MEMBER join sequencer
                                * below. A booted node with VAXCLUSTER!=0 is trying
                                * to JOIN, so it must send the membership request,
                                * not just HELLO-beacon. RUN /DETACHED passes no
                                * env, so we cannot rely on OVMX_JOIN_SEQ here --
                                * this is the in-code equivalent. (The join
                                * sequencer currently reaches step 6/8 and stalls
                                * at JS_MSCP_CONNECT vs a real V7.3 VAX, the
                                * vms-2f3 completion RE -- see scsd.c's join_step
                                * comments; enabling it here turns "HELLO-only, no
                                * request" into "drives the join, stalls at 6/8",
                                * honest progress toward 8/8.) */

        char boot_node[SYSGEN_STRVAL_LEN];
        (void)resolve_node_identity(boot_node, sizeof(boot_node));
        log_ts(stdout);
        printf(" SCSD-I-BOOTCLUSTER, VAXCLUSTER=%u: starting cluster connection"
               " manager on %s, SCSNODE=%s SCSSYSTEMID=%u\n",
               (unsigned)vaxcluster, ifname, boot_node,
               (unsigned)resolve_scssystemid());
        fflush(stdout);
    }

    /* vms-760: the NEW->MEMBER join SEQUENCER (full dir-CLIENT choreography) is
     * gated behind OVMX_JOIN_SEQ, default OFF. It drives OVMX cleanly through 6 of
     * 8 steps against the live lab (own dir connect -> confirm -> lookups ->
     * MSCP$DISK connect) but STALLS at the MSCP$DISK connect on an ESTABLISHED
     * cluster: the established member opens its own dir probe to resolve OVMX and
     * does not process the (byte-perfect) MSCP connect -- its admission gating
     * differs from the fresh-formation reference (formation-clean-2node.pcap),
     * which needs an established-join MSCP capture to ground. With the flag OFF,
     * OVMX keeps the proven VC-connect-first path that reaches SHOW CLUSTER status
     * NEW (Rule 9: no regression). See docs/design-cluster-join-choreography.md. */
    int join_seq_enabled = (getenv("OVMX_JOIN_SEQ") != NULL) || boot_cluster_mode;

    /* --connect implies --respond implies --emit-hello (a beacon is what makes
     * the peer VAX send us the directed HELLO we reply to). */
    if (do_connect) {
        respond = 1;
    }
    if (respond) {
        emit_hello = 1;
    }

    /* vms-838: open + bind the raw-L2 datalink -- AF_PACKET/SOCK_RAW +
     * bind(2) on Linux (unchanged), bpf(4) BIOCSETIF on NetBSD. See
     * scs_datalink.h. */
    int sock = scs_datalink_open(ifname, SCA_ETHERTYPE);
    if (sock < 0) {
        fprintf(stderr,
                "SCSD-E-NOSOCKET, scs_datalink_open('%s') failed: %s\n"
                "  (Linux needs CAP_NET_RAW -- run as root or setcap cap_net_raw+ep"
                " on this binary; NetBSD needs read/write on /dev/bpf*)\n",
                ifname, strerror(errno));
        return 1;
    }

    unsigned ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        fprintf(stderr, "SCSD-E-NOIFACE, unknown interface '%s': %s\n", ifname, strerror(errno));
        scs_datalink_close(sock);
        return 1;
    }

    /* --- vms-b62: resolve HELLO identity. vms-abc removed the send-side
     * sockaddr_ll that used to live here: send_frame_channel() builds it. --- */
    struct scs_hello_params hello_params;
    /* vms-fb1: the receive-dispatch seam's state, including every run counter.
     * Static: the daemon has exactly one, and it keeps the 1514-byte padded
     * HELLO buffer off main()'s stack frame as the old `static pframe` did. */
    static struct scsd_rx rx;
    memset(&rx, 0, sizeof(rx));
    memset(&hello_params, 0, sizeof(hello_params));

    /* vms-5fe responder state, now indexed onto the vms-7be SCA structures. */
    struct peer_state peers[OVMX_MAX_PEERS];
    memset(peers, 0, sizeof(peers));
    /* vms-7be: the VMS SCA Configuration Queue (p. 2-17) and the Port Descriptor
     * Table for OVMX's single local SCA port -- the Ethernet/NISCA port this
     * daemon has bound (p. 2-21: "VMS has one Port Descriptor Table for each
     * local SCA port"). Formative Path Blocks queue here until their circuit
     * opens; open ones move to the System Block of the node they reach. */
    static struct scs_config scs_cfg;
    struct scs_pdt scs_pdt;
    scs_config_init(&scs_cfg);
    scs_pdt_init(&scs_pdt, SCS_PORT_TYPE_ETHERNET, SCS_HELLO_PADDED_MAX_SCA);
    /* vms-dd5: "The CDL is allocated during system initialization" (p. 2-30).
     * The transition log goes to stdout beside every other SCSD-I- line so a run
     * log carries the state history of every connection; if the machine is
     * disabled nothing is initialized and nothing is logged. */
    if (scs_conn_fsm_enabled()) {
        /* vms-578: TELL THE CDL WHICH HIGH HALF THIS INCARNATION ISSUES.
         * worktree-760 made ovmx_conid_base() per-incarnation (vms-2f3: a
         * returning node must not present its previous incarnation's handles),
         * while the CDL validated the tag against the compile-time
         * SCS_CDT_CONID_TAG. Unset, the CDL refuses EVERY real Con.ID and no
         * connection is tracked at all. Set once, here, before anything can
         * allocate. */
        scs_cdt_set_conid_tag((uint16_t)(ovmx_conid_base() >> 16));
        scs_cdl_init(&scsd_cdl);
        scsd_cdl_ready = 1;
        scs_conn_set_log(stdout);
        /* vms-561: bind the service port and declare p. 2-22's "list of
         * listening SYSAPs" -- see scsd_svc(), which does both. Called here so
         * a run log shows the port up before the first frame. */
        (void)scsd_svc();
    } else {
        /* Descriptorless (scs_svc.h): the services still emit, nothing is
         * recorded. scsd_svc_port keeps a NULL CDL so that is what happens. */
        log_ts(stderr);
        fprintf(stderr, " SCSD-I-CONNFSMOFF, OVMX_NO_CONN_FSM is set:"
                        " no connection descriptors, no state machine, no"
                        " transition log, no stuck-connection report\n");
        fflush(stderr);
    }
    /* vms-17f: say once, at startup, exactly what the departure policy is, so a
     * capture is never read as a spontaneous departure (or as the absence of
     * one). A timeout at or below the longest peer silence ever measured on a
     * HEALTHY link is legal -- the lab harness forces one to make a departure
     * happen inside a short run -- but it is never silent. */
    if (!scs_depart_enabled()) {
        log_ts(stderr);
        fprintf(stderr, " SCSD-I-DEPARTOFF, OVMX_NO_PEER_DEPART is set: no peer is"
                        " ever declared departed, no path block is closed and no"
                        " peer slot is released -- the pre-vms-17f behaviour\n");
        fflush(stderr);
    } else {
        uint64_t lt = scs_depart_listen_timeout_ms();
        log_ts(stdout);
        printf(" SCSD-I-DEPARTON, peer listen timeout %llums%s\n",
               (unsigned long long)lt,
               lt <= (uint64_t)SCS_DEPART_HEALTHY_SILENCE_MAX_MS
                   ? " -- AT OR BELOW the longest silence measured on a healthy"
                     " link: departures in this run may be artificial"
                   : "");
        fflush(stdout);
    }
    static const uint8_t lab_nonce[4] = SCS_HELLO_LAB_NONCE_BYTES;
    uint8_t our_hw_mac[6];
    memset(our_hw_mac, 0, sizeof(our_hw_mac));
    uint8_t our_src_logical[6]; /* vms-9f3: OVMX's cluster-LOGICAL LAVC addr (abs 24) */
    memset(our_src_logical, 0, sizeof(our_src_logical));
    /* vms-fb1: every run counter now lives in `rx` so scsd_exit_summary() can
     * report them from a function a test can call.
     *
     * vms-578: worktree-760 kept these as main()-locals and added three of its
     * own -- cm_abort_seen, credit_retx_seen and mscp_srv_accepts. Locals in
     * main() are exactly what the SCSD_UNIT_TEST seam renames out of reach, so
     * the three were MOVED INTO struct scsd_rx rather than kept here; see their
     * declarations there and their lines in scsd_exit_summary(). */

    /* OVMX identity for the phase-2 START/config body (vms-21e). Resolved once;
     * shared by every peer's START responder. */
    char ovmx_node[SYSGEN_STRVAL_LEN];
    if (resolve_node_identity(ovmx_node, sizeof(ovmx_node)) != 0) {
        scs_datalink_close(sock);
        return 1;
    }
    uint16_t ovmx_scssystemid = resolve_scssystemid();
    /* vms-9cf: the allocation class this node adopts from the SYSGEN store.
     * Reported at startup beside the rest of the identity so a run log shows
     * exactly which authored ALLOCLASS the daemon booted with; used for local
     * reporting only -- it is NOT placed on any wire frame (that stays vms-41d's
     * / the cluster-wire session's call) and does not affect the VOTES=0 join. */
    uint8_t ovmx_alloclass = resolve_alloclass();
    log_ts(stdout);
    printf(" SCSD-I-IDENT, SCSNODE=%s SCSSYSTEMID=%u ALLOCLASS=%u\n",
           ovmx_node, (unsigned)ovmx_scssystemid, (unsigned)ovmx_alloclass);
    fflush(stdout);
    /* vms-2f3: have we been admitted to a cluster before under this identity? */
    prior_admission_load();
    /* vms-9f3: OVMX's cluster-LOGICAL LAVC address, computed ONCE from its own
     * SCSSYSTEMID (aa:00:04:00:<LE16(sysid)>). Written at the SCA src-logical
     * field (abs 24) of every emitted frame; the raw HW MAC stays at eth-src
     * and the HELLO HW-MAC tail. This is the fix that lets VAX1's PEDRIVER
     * verify the channel and open an OVMX CSB. */
    ovmx_cluster_logical(ovmx_scssystemid, our_src_logical);

    /* vms-7be: OVMX's OWN System Block, in its own configuration queue.
     * "SCA requires that each node in a network maintain an SB for every node in
     * the network. Consequently, a node must maintain an SB that describes its
     * own CPU and operating system." (p. 2-16; Figure 2-10 on p. 2-18 shows
     * VAX_1's own SB queued alongside the remote ones.) Local bookkeeping only:
     * nothing here is transmitted. */
    {
        struct scs_sb_info self_info;
        memset(&self_info, 0, sizeof(self_info));
        memcpy(self_info.system_id, our_src_logical, SCS_SYSTEM_ID_LEN);
        self_info.node_name = ovmx_node;
        self_info.os_name = "OVMX";
        scs_config_insert_sb(&scs_cfg, &self_info);
    }

    if (emit_hello) {
        uint8_t hw_mac[6];
        if (get_iface_hwaddr(ifname, hw_mac) != 0) {
            fprintf(stderr, "SCSD-E-NOHWADDR, cannot resolve HW address of '%s': %s\n",
                    ifname, strerror(errno));
            scs_datalink_close(sock);
            return 1;
        }

        char node_name[SYSGEN_STRVAL_LEN];
        if (resolve_node_identity(node_name, sizeof(node_name)) != 0) {
            scs_datalink_close(sock);
            return 1;
        }
        uint16_t group = resolve_cluster_group();

        memcpy(our_hw_mac, hw_mac, 6);
        scs_hello_multicast_addr(group, hello_params.dst_mac);
        memcpy(hello_params.src_mac, hw_mac, 6);
        memcpy(hello_params.src_logical, our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
        strncpy(hello_params.node_name, node_name, SCS_HELLO_NODENAME_LEN);
        hello_params.node_name[SCS_HELLO_NODENAME_LEN] = '\0';

        log_ts(stderr);
        fprintf(stderr,
                " SCSD-I-HELLOCFG, node='%s' group=%u mcast=%02x:%02x:%02x:%02x:%02x:%02x"
                " hwmac=%02x:%02x:%02x:%02x:%02x:%02x interval=%ds\n",
                node_name, group,
                hello_params.dst_mac[0], hello_params.dst_mac[1], hello_params.dst_mac[2],
                hello_params.dst_mac[3], hello_params.dst_mac[4], hello_params.dst_mac[5],
                hw_mac[0], hw_mac[1], hw_mac[2], hw_mac[3], hw_mac[4], hw_mac[5],
                hello_interval);

        /* Bound recv() so the loop wakes periodically to check the HELLO
         * timer even when the wire is idle. vms-838: SO_RCVTIMEO on Linux,
         * BIOCSRTIMEOUT on NetBSD -- see scs_datalink_set_recv_timeout(). */
        if (scs_datalink_set_recv_timeout(sock, 1) < 0) {
            fprintf(stderr, "SCSD-E-RCVTIMEO, scs_datalink_set_recv_timeout failed: %s\n", strerror(errno));
            scs_datalink_close(sock);
            return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (duration > 0) {
        signal(SIGALRM, on_signal);
        alarm((unsigned)duration);
    }

    log_ts(stderr);
    fprintf(stderr,
            " SCSD-I-LISTEN, raw socket bound to '%s' ethertype 0x%04x (%s)%s\n",
            ifname, SCA_ETHERTYPE,
            duration > 0 ? "bounded run" : "run until SIGINT/SIGTERM",
            emit_hello ? ", HELLO beacon enabled" : "");

    static uint8_t buf[SCA_FRAME_MAX];
    struct timespec last_hello = {0, 0};
    /* vms-2f3 step 4 / vms-ebb: how long to wait for the member's op 6 before
     * starting the disk-discovery run anyway now lives in
     * scsd_diskrun_gate_ms(), read by scsd_diskrun_ungate_tick() itself. */

    /* vms-4071: everything the VC formation state machine needs to put its
     * actions on the wire. Pointers, so the HW MAC resolved in the emit_hello
     * block above stays live. */
    struct scsd_vc_ctx vc_ctx;
    vc_ctx.sock = sock;
    vc_ctx.ifindex = (int)ifindex;
    vc_ctx.hw_mac = our_hw_mac;
    vc_ctx.src_logical = our_src_logical;
    vc_ctx.scssystemid = ovmx_scssystemid;
    vc_ctx.node_name = ovmx_node;
    vc_ctx.cfg = &scs_cfg;

    /* vms-fb1: wire the receive-dispatch seam. Every field below is a pointer
     * to state main() owns; the handler reaches ALL of it through `rx` and
     * holds no state of its own, which is what lets a test stand one up. */
    rx.sock = sock;
    rx.ifindex = (int)ifindex;
    rx.our_hw_mac = our_hw_mac;
    rx.our_src_logical = our_src_logical;
    rx.lab_nonce = lab_nonce;
    rx.hello_params = &hello_params;
    rx.cfg = &scs_cfg;
    rx.pdt = &scs_pdt;
    rx.peers = peers;
    rx.vc_ctx = &vc_ctx;
    rx.ifname = ifname;
    rx.respond = respond;
    rx.do_connect = do_connect;
    rx.emit_hello = emit_hello;
    rx.join_seq_enabled = join_seq_enabled; /* vms-578: travels with the context */

    /* vms-2f3: THE WHITE-WHALE FIX. If this identity was admitted before and the
     * prior run did NOT record a clean shutdown, it CRASHED -- the peer may still
     * hold our prior incarnation's VMS$VAXcluster CDT in reconnect/long_break for
     * ~RECNXINTERVAL (~100 s), refusing our return onto that still-open CSB. Emit
     * that crashed incarnation's port-level last gasp NOW, at birth, before the
     * fresh join, so the peer removes the corpse in seconds. No-op on a clean
     * rejoin or a first join. See scsd_emit_crash_lastgasp(). */
    (void)scsd_emit_crash_lastgasp(&rx);

    while (!g_stop) {
        if (emit_hello) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - last_hello.tv_sec >= hello_interval) {
                /* vms-abc: the beacon's build-and-send is scsd_hello_beacon_emit()
                 * -- see that function. Only the CADENCE is here. It used to be
                 * inline AND it used to call sendto() on `sock` directly, which
                 * put a real send site (it increments rx.hello_sent and is
                 * reported in the exit summary) in NEITHER half of a SEND SITE
                 * TABLE that claims to list every sender, inside the one
                 * function no test can reach. The wire is unchanged: the same
                 * frame goes to the same multicast MAC on the same interface at
                 * the same times, because send_frame_raw() builds the same
                 * sockaddr_ll the deleted `hello_dst` held. */
                (void)scsd_hello_beacon_emit(&rx, &hello_params, sock, (int)ifindex);
                last_hello = now;
            }
        }

        /* --- vms-17f: the peer departure sweep. A peer silent past the listen
         * timeout has its Path Block torn down (p. 2-28 order) and its slot
         * released, so that if it comes back it re-drives formation and its
         * open takes the p. 2-21 REFRESH. WIRE-VISIBLE; OVMX_NO_PEER_DEPART=1
         * disables it entirely. See the block above scsd_peer_departure_sweep. */
        (void)scsd_peer_departure_sweep(&rx, monotonic_ms());

        /* --- vms-4071: the p. 2-14 formation reissue timer, moved to a
         * function (vms-fb1) so the SCSD_UNIT_TEST seam can reach it -- see
         * scsd_vc_reissue_tick() above. THIS IS NEW ON THE WIRE and fires only
         * under loss; the OVMX_VC_NO_RETRY_LIMIT=1 kill-switch restores
         * unbounded retry. */
        (void)scsd_vc_reissue_tick(&rx, monotonic_ms());

        /* --- vms-66f: the p. 2-50 SCS process poll, moved to a function
         * (vms-fb1) so the SCSD_UNIT_TEST seam can reach it -- see
         * scsd_poll_refresh_tick() above. WIRE-VISIBLE and NEW: this is the
         * first outbound SCS$DIRECTORY traffic OVMX has ever emitted.
         * OVMX_NO_PROCESS_POLLER=1 suppresses it entirely. */
        scsd_poll_refresh_tick(&rx, monotonic_ms());

        /* --- vms-691: retransmit OVMX's own unacked sequenced message (the
         * connect-request) on timeout, so a dropped CONNECT-REQUEST does not
         * stall the handshake. Rate-capped (VC_RETRANSMIT_MAX) to respect the
         * reply-amplification guard. Only while the connect is still unbound. */
        /* vms-abc moved the body into scsd_retransmit_tick() so a test can
         * reach it, and made retransmit exhaustion break the circuit. */
        scsd_retransmit_tick(&rx, monotonic_ms());

        /* --- vms-694: the join sequencer's own per-step retransmit (JS_DIR_
         * CONNECT / JS_MSCP_CONNECT / JS_VC_CONNECT / ...), now ALSO driven
         * here on the loop's own clock rather than solely from a directed
         * HELLO received from the stalled peer. See scsd_join_retx_tick()'s
         * header for the lab evidence. OVMX_NO_JOIN_RETX_TICK=1 disables it. */
        scsd_join_retx_tick(&rx, monotonic_ms());

        /* --- vms-c7d: the RECNXINTERVAL reconnect loop. For a peer whose VC
         * broke (not a last gasp), retry the connection once a second and HOLD
         * its membership for max(RECNXINTERVAL, remote) seconds before proposing
         * a transition (transcript p. 7-30). Inert unless a CSB is in
         * WAIT/RECONNECT. OVMX_NO_RECNX_TICK=1 disables it. */
        scsd_recnx_tick(&rx, monotonic_ms());

        /* --- vms-8d4: drive the executive's live member set (throttled to
         * ~1 Hz inside) so SHOW CLUSTER / F$GETSYI read real membership
         * instead of a hardcoded NOTMEMBER. See docs/design-cluster-node.md
         * §3.1 and docs/design-cluster-membership-executive.md (vms-551). */
        scsd_publish_membership(&rx, monotonic_ms());

        /* --- DISK DISCOVERY HAS EXACTLY ONE TRIGGER, AND THIS CALL IS IT.
         * vms-ebb moved the body into scsd_diskrun_ungate_tick() so a test can
         * reach it (SCSD_UNIT_TEST renames main() away), and ruled the single
         * trigger on a lab bracket -- spec sec 4(O.4). Read that function's
         * header before adding a second entry point. */
        (void)scsd_diskrun_ungate_tick(&rx, monotonic_ms());

        ssize_t n = scs_datalink_recv(sock, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue; /* SIGALRM/SIGINT -- g_stop checked at loop top */
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue; /* SO_RCVTIMEO wakeup with nothing pending -- re-check HELLO timer */
            }
            fprintf(stderr, "SCSD-E-RECVFAIL, recv failed: %s\n", strerror(errno));
            break;
        }
        /* vms-578 INTEGRATION: worktree-760 kept the whole per-frame dispatch
         * INLINE here, and grew ~2000 lines of connection-manager handling
         * inside it. All of it has been merged into scsd_handle_frame() above,
         * section by section against their common ancestor -- see the vms-578
         * notes there for every place the two sides genuinely competed. The
         * inline copy is deleted because there can be only one dispatcher, not
         * because anything in it was dropped.
         *
         * The exit summary that trailed it is likewise scsd_exit_summary()`s;
         * the three counters worktree-760 added to it (CM-OP04-BARRIER-SEEN,
         * CREDIT-RETX-ANSWERED, MSCP-SERVER-ACCEPTS-SENT) moved into
         * struct scsd_rx and are printed from there. */
        scsd_handle_frame(&rx, buf, n);
    }

    /* vms-591: disconnect before vanishing. Bounded; see the function. */
    scsd_shutdown_teardown(&rx, buf, sizeof(buf));

    scsd_exit_summary(&rx, stderr);

    scs_datalink_close(sock);
    return 0;
}
