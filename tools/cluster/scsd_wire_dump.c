/*
 * scsd_wire_dump.c - dump EVERY frame src/vmsscs/scsd.c transmits for a fixed
 * replay script, so two revisions of scsd.c can be byte-diffed against each
 * other (vms-561).
 *
 * WHY THIS EXISTS. vms-561 moved connection formation off open-coded frame
 * emission and onto the five architected SCS services. The item's constraint is
 * that the frames on the wire MUST be unchanged for the existing happy paths,
 * "proved with a before/after capture diff". This program is that diff's
 * instrument, and it is deliberately built from the daemon's PRODUCTION code
 * path -- not the SCSD_UNIT_TEST one:
 *
 *   - it #includes scsd.c with `sendto` macro-renamed to a local capture
 *     function, so the bytes recorded are the bytes handed to the socket by the
 *     REAL send_frame_raw(), sockaddr_ll and all;
 *   - it renames main() the same way tests do, and supplies its own;
 *   - it defines NOTHING else, and patches NOTHING. It compiles against an
 *     UNMODIFIED scsd.c, which is what lets it be built against a checkout of
 *     the pre-migration tree as easily as against this one. That property is
 *     the whole point: a harness that had to be added to the "before" tree
 *     could not prove anything about the "before" tree.
 *
 * The existing test_scsd_wire.c capture seam records only the LAST frame per
 * call, so it cannot see the first of the two frames the SCS$DIRECTORY accept
 * emits. This sees every one, in order, with its destination MAC.
 *
 * USAGE:  scsd_wire_dump <outfile>
 *         tools/cluster/scsd_wire_diff.sh <before-tree> <after-tree>
 *
 * The replay script below drives the THREE migrated call sites -- the
 * SCS$DIRECTORY accept, the joiner CONNECT (both its p. 2-47 selection paths
 * and a retransmit), and the member-opened VMS$VAXcluster accept including a
 * re-answer -- plus the receive paths that surround them. Frames are the real
 * captured ones; provenance is on each array.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
/*
 * vms-838: this capture shim casts sendto()'s `to` to `struct sockaddr_ll`
 * (below) to print the destination MAC. Before vms-838 that type reached us
 * transitively through SCSD_SOURCE (scsd.c #included <netpacket/packet.h>);
 * the raw-L2 send now lives in scs_datalink.c, so scsd.c no longer pulls it
 * in and this tool must own the include for the type it directly uses.
 */
#include <netpacket/packet.h>

static ssize_t scsd_wire_capture_sendto(int fd, const void *buf, size_t len, int flags,
                                        const struct sockaddr *to, socklen_t tolen);

/* Redirect the daemon's transmit primitive and entry point. Both are plain
 * identifiers in scsd.c, so the preprocessor is enough and the source is
 * untouched. */
#define sendto scsd_wire_capture_sendto
#define main   scsd_wire_daemon_main
/*
 * WHICH scsd.c. The default is this repository's, by a path relative to THIS
 * file, so the tool compiles wherever a whole-tree scan compiles every product
 * source (tests/integration/test_userspace_service_register.sh does exactly
 * that, and a source that does not compile is a red there, correctly -- it
 * would be a place a service could hide). scsd_wire_diff.sh overrides it with
 * -DSCSD_SOURCE to point at ANOTHER checkout, which is the whole comparison.
 */
#ifndef SCSD_SOURCE
#define SCSD_SOURCE "../../src/vmsscs/scsd.c"
#endif
#include SCSD_SOURCE
#undef main
#undef sendto

static FILE *dump_out = NULL;
static unsigned dump_seq = 0;

static ssize_t scsd_wire_capture_sendto(int fd, const void *buf, size_t len, int flags,
                                        const struct sockaddr *to, socklen_t tolen)
{
    const struct sockaddr_ll *sll = (const struct sockaddr_ll *)(const void *)to;
    const unsigned char *p = (const unsigned char *)buf;
    (void)fd;
    (void)flags;
    (void)tolen;

    fprintf(dump_out, "FRAME %04u len=%zu dst=%02x:%02x:%02x:%02x:%02x:%02x\n",
            dump_seq++, len,
            sll->sll_addr[0], sll->sll_addr[1], sll->sll_addr[2],
            sll->sll_addr[3], sll->sll_addr[4], sll->sll_addr[5]);
    for (size_t i = 0; i < len; i++) {
        fprintf(dump_out, "%02x%s", p[i], ((i % 16) == 15 || i + 1 == len) ? "\n" : " ");
    }
    return (ssize_t)len;
}

/* Every frame is transcribed from tests/vmsscs/test_scsd_wire.c, which carries
 * the pcap provenance for each (rule 8: lab observation only). */
static const uint8_t cap_dir_connect_req[124] = {
    0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,
    0x60, 0x07, 0x6c, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x5b, 0x13, 0x00, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x42, 0x00, 0x04, 0x00,
    0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x05, 0x63,
    0x00, 0x00, 0x01, 0x00, 0x53, 0x43, 0x53, 0x24, 0x44, 0x49, 0x52, 0x45,
    0x43, 0x54, 0x4f, 0x52, 0x59, 0x20, 0x20, 0x20, 0x53, 0x43, 0x53, 0x24,
    0x44, 0x49, 0x52, 0x5f, 0x4c, 0x4f, 0x4f, 0x4b, 0x55, 0x50, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20
};

static const uint8_t cap_connect_rsp[80] = {
    0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9,
    0x60, 0x07, 0x40, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x5b, 0x13, 0x01, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x12, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x16, 0x00, 0x04, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x05, 0x63, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x53, 0x43, 0x53, 0x24
};

static const uint8_t cap_vaxcluster_connect_req[124] = {
    0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,
    0x60, 0x07, 0x6c, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x4b, 0x13, 0x06, 0x00, 0x07, 0x00,
    0x01, 0x00, 0x12, 0x00, 0x06, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
    0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x42, 0x00, 0x04, 0x00,
    0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0xc5, 0x62,
    0x00, 0x00, 0x01, 0x00, 0x56, 0x4d, 0x53, 0x24, 0x56, 0x41, 0x58, 0x63,
    0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x20, 0x20, 0x56, 0x4d, 0x53, 0x24,
    0x56, 0x41, 0x58, 0x63, 0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x20, 0x20,
    0x01, 0x1b, 0x01, 0x03, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x08,
    0x00, 0x00, 0x06, 0x00
};

static const uint8_t cap_ovmx_joiner_connect_rsp[80] = {
    0xb6, 0x16, 0x8a, 0xdc, 0x3a, 0x53, 0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9,
    0x60, 0x07, 0x40, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x9b, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x4b, 0x13, 0x0b, 0x00, 0x0b, 0x00,
    0x01, 0x00, 0x12, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00,
    0x0b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x16, 0x00, 0x04, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x58, 0x4f, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x56, 0x4d, 0x53, 0x24
};

static const uint8_t cap_ovmx_joiner_accept_req[124] = {
    0xb6, 0x16, 0x8a, 0xdc, 0x3a, 0x53, 0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9,
    0x60, 0x07, 0x6c, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x9b, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x4b, 0x13, 0x0b, 0x00, 0x0c, 0x00,
    0x01, 0x00, 0x12, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00,
    0x0b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x42, 0x00, 0x04, 0x00,
    0x02, 0x00, 0x0a, 0x00, 0x02, 0x00, 0x58, 0x4f, 0x11, 0x00, 0x02, 0x63,
    0x00, 0x00, 0x00, 0x00, 0x56, 0x4d, 0x53, 0x24, 0x56, 0x41, 0x58, 0x63,
    0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x20, 0x20, 0x56, 0x4d, 0x53, 0x24,
    0x56, 0x41, 0x58, 0x63, 0x6c, 0x75, 0x73, 0x74, 0x65, 0x72, 0x20, 0x20,
    0x01, 0x1b, 0x01, 0x03, 0x01, 0x00, 0x01, 0x00, 0x03, 0x00, 0x01, 0x08,
    0x00, 0x00, 0x06, 0x00
};


/*
 * vms-7fe: TWO REAL SCS$DIR_LOOKUP REQUESTS, byte-exact from
 * formation-ci1-joinwindow.pcap (the same SCA#29 / SCA#37 payloads
 * tests/vmsscs/test_scs_dir.c has carried since vms-246), with a 14-byte
 * Ethernet header prepended. These are the two shapes the responder answers:
 *   SCA#29  MSCP$TAPE       opcode 0x5b -- NOT in OVMX's SDIR queue -> the
 *                           GROUNDED "NOT PRESENT HERE" marker
 *   SCA#37  VMS$VAXcluster  opcode 0x4b -- IS in the queue -> affirmative
 * The whole point of replaying them here is that vms-7fe replaced the hardcoded
 * name compare that used to decide both answers with a scan of the SDIR queue,
 * and BOTH ANSWERS MUST STILL BE THE SAME BYTES.
 */
static const uint8_t cap_lookup_req_mscptape[108] = {
    0x08,0x00,0x2b,0x78,0x56,0xb9, 0xaa,0x00,0x04,0x00,0x01,0x04, 0x60,0x07,
    0x5c,0x00,0xaa,0x00,0x04,0x00,0x02,0x04,0x01,0x00,0xaa,0x00,0x04,0x00,0x01,0x04,
    0x5b,0x13,0x02,0x00,0x03,0x00,0x01,0x00,0x12,0x00,0x02,0x00,0x00,0x00,0x03,0x00,
    0x00,0x00,0x02,0x00,0x00,0x00,0x01,0x00,0x00,0x02,0x32,0x00,0x04,0x00,0x0a,0x00,
    0x00,0x00,0x07,0x00,0x59,0x33,0x08,0x00,0x05,0x63,0x00,0x00,0x00,0x00,0x4d,0x53,
    0x43,0x50,0x24,0x54,0x41,0x50,0x45,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static const uint8_t cap_lookup_req_vaxcluster[108] = {
    0x08,0x00,0x2b,0x78,0x56,0xb9, 0xaa,0x00,0x04,0x00,0x01,0x04, 0x60,0x07,
    0x5c,0x00,0xaa,0x00,0x04,0x00,0x02,0x04,0x01,0x00,0xaa,0x00,0x04,0x00,0x01,0x04,
    0x4b,0x13,0x05,0x00,0x06,0x00,0x01,0x00,0x12,0x00,0x05,0x00,0x00,0x00,0x06,0x00,
    0x00,0x00,0x05,0x00,0x00,0x00,0x01,0x00,0x00,0x02,0x32,0x00,0x04,0x00,0x0a,0x00,
    0x00,0x00,0x07,0x00,0x59,0x33,0x08,0x00,0x05,0x63,0x00,0x00,0x00,0x00,0x56,0x4d,
    0x53,0x24,0x56,0x41,0x58,0x63,0x6c,0x75,0x73,0x74,0x65,0x72,0x20,0x20,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static const uint8_t vax2_hw_mac[6] = {0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9};
static const uint8_t vax1_hw_mac[6] = {0xaa, 0x00, 0x04, 0x00, 0x01, 0x04};
static const uint8_t vax1_logical[6] = {0xaa, 0x00, 0x04, 0x00, 0x01, 0x04};
static const uint8_t ovmx760_hw_mac[6] = {0xb6, 0x16, 0x8a, 0xdc, 0x3a, 0x53};
static const uint8_t ovmx760_logical[6] = {0xaa, 0x00, 0x04, 0x00, 0x02, 0x04};
static const uint8_t ovmx760_member_mac[6] = {0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9};
static const uint8_t ovmx760_member_sysid[6] = {0xaa, 0x00, 0x04, 0x00, 0x9b, 0x04};
static const uint8_t our_logical[6] = {0xaa, 0x00, 0x04, 0x00, 0x41, 0x05};

struct dumpworld {
    struct scs_config cfg;
    struct scs_pdt pdt;
    struct peer_state peers[OVMX_MAX_PEERS];
    struct scs_hello_params hello_params;
    struct scsd_vc_ctx vc_ctx;
    struct scsd_rx rx;
    uint8_t hw_mac[6];
    uint8_t logical[6];
    uint8_t nonce[4];
};

static void dumpworld_init(struct dumpworld *w, const uint8_t hw_mac[6],
                           const uint8_t logical[6])
{
    static const uint8_t lab_nonce[4] = SCS_HELLO_LAB_NONCE_BYTES;
    memset(w, 0, sizeof(*w));
    scs_config_init(&w->cfg);
    scs_pdt_init(&w->pdt, SCS_PORT_TYPE_ETHERNET, SCA_FRAME_MAX);
    scs_cdl_init(&scsd_cdl);
    /* vms-7fe: the port's SDIR queue names listening CDTs out of this CDL, so
     * re-initializing one without the other leaves a queue pointing at wiped
     * slots. The daemon initializes its CDL once; only this harness rebuilds a
     * world, so only this harness has to reset both. (The pre-vms-7fe tree has
     * no scs_svc_port field to reset -- the memset is over the whole struct.) */
    memset(&scsd_svc_port, 0, sizeof(scsd_svc_port));
    scsd_cdl_ready = 1;
    memcpy(w->hw_mac, hw_mac, 6);
    memcpy(w->logical, logical, 6);
    memcpy(w->nonce, lab_nonce, 4);

    w->vc_ctx.sock = 7;
    w->vc_ctx.ifindex = 1;
    w->vc_ctx.hw_mac = w->hw_mac;
    w->vc_ctx.src_logical = w->logical;
    w->vc_ctx.scssystemid = 1329;
    w->vc_ctx.node_name = "OVMX";
    w->vc_ctx.cfg = &w->cfg;

    w->rx.sock = 7;
    w->rx.ifindex = 1;
    w->rx.our_hw_mac = w->hw_mac;
    w->rx.our_src_logical = w->logical;
    w->rx.lab_nonce = w->nonce;
    w->rx.hello_params = &w->hello_params;
    w->rx.cfg = &w->cfg;
    w->rx.pdt = &w->pdt;
    w->rx.peers = w->peers;
    w->rx.vc_ctx = &w->vc_ctx;
    w->rx.ifname = "test0";
    w->rx.respond = 1;
    w->rx.do_connect = 1;
    w->rx.emit_hello = 0;
}

static struct peer_state *open_circuit_to(struct dumpworld *w, const uint8_t mac[6],
                                          const uint8_t sys_addr[6])
{
    struct peer_state *ps = peer_find_or_add(&w->cfg, &w->pdt, w->peers, mac);
    if (ps == NULL) {
        return NULL;
    }
    ps_learn_sys_addr(&w->cfg, ps, sys_addr);
    (void)scs_pb_open(&w->cfg, ps->pb);
    return ps;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <outfile>\n", argv[0]);
        return 2;
    }
    dump_out = fopen(argv[1], "w");
    if (dump_out == NULL) {
        perror("fopen");
        return 2;
    }
    /* The daemon's own logging carries timestamps and is not part of the
     * comparison; send it where it cannot pollute the dump. */
    if (freopen("/dev/null", "w", stdout) == NULL ||
        freopen("/dev/null", "w", stderr) == NULL) {
        return 2;
    }

    /* --- (1) SCS$DIRECTORY ACCEPT: two frames (op=1 echo, op=2 response), plus
     * the joiner CONNECT-REQUEST it triggers once START has been acked. --- */
    {
        struct dumpworld w;
        dumpworld_init(&w, vax2_hw_mac, our_logical);
        struct peer_state *ps = open_circuit_to(&w, vax1_hw_mac, vax1_logical);
        if (ps != NULL) {
            ps->start_acked = 1;
        }
        fprintf(dump_out, "== case 1: SCS$DIRECTORY accept + prompt joiner connect\n");
        scsd_handle_frame(&w.rx, cap_dir_connect_req, (ssize_t)sizeof(cap_dir_connect_req));
        /* A REPEAT of the same request: the branch is latched on dir_connected,
         * so this must add nothing. */
        scsd_handle_frame(&w.rx, cap_dir_connect_req, (ssize_t)sizeof(cap_dir_connect_req));
    }

    /* --- (2) member-opened VMS$VAXcluster ACCEPT: the 0x4b CONNECT-RESPONSE,
     * then a RE-ANSWER of a retransmitted request. --- */
    {
        struct dumpworld w;
        dumpworld_init(&w, vax2_hw_mac, our_logical);
        (void)open_circuit_to(&w, vax1_hw_mac, vax1_logical);
        fprintf(dump_out, "== case 2: member VMS$VAXcluster accept + re-answer\n");
        scsd_handle_frame(&w.rx, cap_vaxcluster_connect_req,
                          (ssize_t)sizeof(cap_vaxcluster_connect_req));
        scsd_handle_frame(&w.rx, cap_vaxcluster_connect_req,
                          (ssize_t)sizeof(cap_vaxcluster_connect_req));
    }

    /* --- (3) the joiner CONNECT service, both p. 2-47 paths and a retransmit,
     * driven through the production sender directly. --- */
    {
        struct dumpworld w;
        dumpworld_init(&w, vax2_hw_mac, our_logical);
        struct peer_state *ps = open_circuit_to(&w, vax1_hw_mac, vax1_logical);
        fprintf(dump_out, "== case 3: CONNECT named / selected / retransmit\n");
        if (ps != NULL) {
            scs_vc_reset_seq(&ps->vc);
            (void)send_joiner_connect_request(7, 1, &w.cfg, ps, ps->pb,
                                              w.hw_mac, w.logical);
            (void)send_joiner_connect_request(7, 1, &w.cfg, ps, NULL,
                                              w.hw_mac, w.logical);
            (void)send_joiner_connect_request(7, 1, &w.cfg, ps, NULL,
                                              w.hw_mac, w.logical);
        }
    }

    /* --- (4) OVMX AS THE JOINER: the member's CONNECT_RSP then ACCEPT_REQ for
     * OVMX's own connection, which drives the add-member config burst. --- */
    {
        struct dumpworld w;
        dumpworld_init(&w, ovmx760_hw_mac, ovmx760_logical);
        struct peer_state *ps = open_circuit_to(&w, ovmx760_member_mac, ovmx760_member_sysid);
        fprintf(dump_out, "== case 4: joiner accepted by the member (CM burst)\n");
        if (ps != NULL) {
            scs_vc_reset_seq(&ps->vc);
            (void)send_joiner_connect_request(7, 1, &w.cfg, ps, NULL,
                                              w.hw_mac, w.logical);
        }
        scsd_handle_frame(&w.rx, cap_ovmx_joiner_connect_rsp,
                          (ssize_t)sizeof(cap_ovmx_joiner_connect_rsp));
        scsd_handle_frame(&w.rx, cap_ovmx_joiner_accept_req,
                          (ssize_t)sizeof(cap_ovmx_joiner_accept_req));
    }

    /* --- (5) THE SAME SCRIPT WITH THE STATE MACHINE OFF. The kill switch must
     * change no byte either -- descriptorless mode exists precisely so that the
     * services still emit what the pre-service code emitted. --- */
    (void)setenv("OVMX_NO_CONN_FSM", "1", 1);
    {
        struct dumpworld w;
        dumpworld_init(&w, vax2_hw_mac, our_logical);
        struct peer_state *ps = open_circuit_to(&w, vax1_hw_mac, vax1_logical);
        if (ps != NULL) {
            ps->start_acked = 1;
        }
        fprintf(dump_out, "== case 5: same, OVMX_NO_CONN_FSM=1\n");
        scsd_handle_frame(&w.rx, cap_dir_connect_req, (ssize_t)sizeof(cap_dir_connect_req));
        scsd_handle_frame(&w.rx, cap_vaxcluster_connect_req,
                          (ssize_t)sizeof(cap_vaxcluster_connect_req));
        scsd_handle_frame(&w.rx, cap_vaxcluster_connect_req,
                          (ssize_t)sizeof(cap_vaxcluster_connect_req));
    }
    (void)unsetenv("OVMX_NO_CONN_FSM");


    /* --- (6) vms-7fe: THE SCS$DIR_LOOKUP RESPONDER, whose decision moved from
     * a hardcoded name compare to the p. 2-48 SDIR scan. Both real captured
     * queries are replayed: MSCP$TAPE (not listening -> "NOT PRESENT HERE") and
     * VMS$VAXcluster (listening -> affirmative). If the rewire changed either
     * answer, or any other byte of either response, this case reds the diff.
     * Run twice, so the second pass also covers the responder on an already
     * dir_connected peer. --- */
    {
        struct dumpworld w;
        dumpworld_init(&w, vax2_hw_mac, our_logical);
        struct peer_state *ps = open_circuit_to(&w, vax1_hw_mac, vax1_logical);
        if (ps != NULL) {
            ps->start_acked = 1;
        }
        fprintf(dump_out, "== case 6: SCS$DIR_LOOKUP miss + hit\n");
        scsd_handle_frame(&w.rx, cap_dir_connect_req, (ssize_t)sizeof(cap_dir_connect_req));
        /* The golden lookups sit several credit shorts after the SCA#21
         * connect, so a contiguous replay has to renumber them or the daemon's
         * own p. 2-31 sequentiality check correctly breaks the circuit on the
         * gap and nothing further is emitted at all. spec sec 4(h)(4) grounds
         * send_seq [20:22] + its [30:32] mirror as state a sender COMPUTES. */
        for (uint16_t seq = 2; seq <= 5; seq++) {
            uint8_t f[108];
            const uint8_t *src = (seq % 2) == 0 ? cap_lookup_req_mscptape
                                                : cap_lookup_req_vaxcluster;
            memcpy(f, src, sizeof(f));
            f[14 + 20] = (uint8_t)(seq & 0xff);
            f[14 + 21] = (uint8_t)(seq >> 8);
            f[14 + 30] = (uint8_t)(seq & 0xff);
            f[14 + 31] = (uint8_t)(seq >> 8);
            scsd_handle_frame(&w.rx, f, (ssize_t)sizeof(f));
        }
    }

    fprintf(dump_out, "== end, %u frame(s)\n", dump_seq);
    fclose(dump_out);
    return 0;
}
