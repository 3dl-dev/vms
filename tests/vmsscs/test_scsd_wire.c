/*
 * test_scsd_wire.c - vms-7be: the SCSD-LEVEL test for the SB/PB refactor.
 *
 * WHY THIS FILE EXISTS. vms-7be moved every fact SCSD knows about a peer out of
 * an ad-hoc MAC-keyed struct and into architected System Blocks and Path Blocks.
 * tests/vmsscs/test_scs_config.c covers the new module in isolation, but 100% of
 * the WIRE-REACHING change lives in src/vmsscs/scsd.c, which until now had zero
 * automated coverage: the nine other scs tests link the builder libraries and
 * never compile or run a line of scsd.c. "The refactor is wire-invisible" rested
 * on reading the diff, which is not a test. This file makes it a test.
 *
 * HOW. This translation unit #includes src/vmsscs/scsd.c with SCSD_UNIT_TEST
 * defined. That does exactly two things to the daemon source (both guarded, both
 * absent from the shipped binary):
 *   1. send_frame_to() captures the frame into scsd_test_last_frame/_dst instead
 *      of calling sendto() on an AF_PACKET socket that needs CAP_NET_RAW. The
 *      frame handed to it is built by the REAL scsd.c senders from the REAL
 *      peer_state accessors -- nothing is re-derived here.
 *   2. the daemon's main() is RENAMED (not compiled out) so this file can supply
 *      its own and every static helper is still compiled as the daemon compiles it.
 * Everything under test -- peer_find_or_add, ps_learn_sys_addr, ps_sys_addr,
 * ps_port_addr, send_joiner_connect_request -- is the production code itself.
 *
 * THE TWO RISKS THIS CLOSES (raised against the vms-7be diff):
 *   (a) ps_sys_addr() now reads a SHARED struct scs_sb, so two peer_states whose
 *       Path Blocks resolve to one System ID alias an address that used to be a
 *       per-peer copy. Pinned in test_shared_sb_aliases_the_peer_logical().
 *   (b) scs_pb_learn_system_addr() can return NULL on SB-pool exhaustion where
 *       the old code's memcpy always stored, which would put a ZERO peer-logical
 *       on the wire. Pinned in test_sb_exhaustion_is_visible_and_unreachable():
 *       the degraded frame is asserted, the failure is asserted to be LOGGED (not
 *       silent), and the path is shown unreachable at the shipped pool sizes.
 *
 * ALSO PINNED HERE (vms-398): CONNECT's choice of virtual circuit. The daemon's
 * CONNECT-REQUEST call sites name a NODE, not a circuit, so the circuit is
 * selected from the configuration database (CONFIG_SYS + the p. 2-47 OPEN scan).
 * test_connect_selects_the_open_vc_via_config_sys() drives that production
 * sender and asserts the selection is wire-invisible, refuses honestly when no
 * circuit is OPEN, and follows circuit STATE rather than peer-slot identity.
 *
 * ALSO PINNED HERE (vms-dd5 + vms-fb1): the connection state machine as the
 * DAEMON drives it, and -- since vms-fb1 hoisted the per-frame dispatch out of
 * main() into scsd_handle_frame() -- the RECEIVE side as well. The six
 * test_captured_* / test_null_source_* / test_exit_summary_* cases at the
 * bottom of this file feed scsd_handle_frame() frames transcribed byte-exact
 * from formation-ci1-joinwindow.pcap (the VAX-to-VAX golden formation) and
 * from ovmx-760-MEMBER-achieved-20260730.pcap (the run in which OVMX ITSELF
 * joined, whose frames are already addressed to OVMX's own Con.IDs and so need
 * no edit at all), and assert the resulting CDT state and CONID. Every state
 * transition asserted there is performed by scsd.c: no case calls conn_step()
 * or scs_cdt_set_remote_conid() on its own behalf.
 *
 * SCOPE HONESTY, restated to match what is actually here: this exercises SCSD's
 * frame-assembly path AND its per-frame receive dispatch, through the production
 * functions, with the transmit call and the socket replaced by the capture seam.
 * It does NOT exercise socket setup, the pre-recv timer blocks in main()'s loop
 * (the VC reissue timer and the vms-691 retransmit timer are still reachable
 * only from main()), or any real interface. A lab join capture is the end-to-end
 * proof and is a separate activity; nothing here claims to be one.
 */
#include <stdio.h>
#include <string.h>

/* The seam. Must precede the include of the daemon source. */
#define SCSD_UNIT_TEST 1
#include "../../src/vmsscs/scsd.c"

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

/* The shipped pool sizes must leave no way for the SB allocator to fail on the
 * peer path: one formative SB per peer plus the local node's own SB (p. 2-16). */
_Static_assert(SCS_CONFIG_MAX_PB >= OVMX_MAX_PEERS,
               "PB pool cannot hold OVMX_MAX_PEERS peers");
_Static_assert(SCS_CONFIG_MAX_SB >= OVMX_MAX_PEERS + 1,
               "SB pool cannot hold one SB per peer plus the local node's own SB");

/* Lab-shaped 48-bit SCS System Address: aa:00:04:00:<LE16(SCSSYSTEMID)>. */
static void sysid_of(uint16_t scssystemid, uint8_t out[6])
{
    out[0] = 0xaa;
    out[1] = 0x00;
    out[2] = 0x04;
    out[3] = 0x00;
    out[4] = (uint8_t)(scssystemid & 0xff);
    out[5] = (uint8_t)((scssystemid >> 8) & 0xff);
}

/* For the diff experiments below the two addresses must differ in EVERY byte, so
 * that "exactly six bytes changed" is a real localization and not an artifact of
 * two lab-shaped addresses sharing their aa:00:04:00 prefix. */
static const uint8_t addr_x[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
static const uint8_t addr_y[6] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6};
static const uint8_t port_x[6] = {0x08, 0x00, 0x2b, 0x0a, 0x0b, 0x0c};
static const uint8_t port_y[6] = {0x00, 0x1b, 0x38, 0x71, 0x62, 0x93};

static void mac_of(uint8_t last, uint8_t out[6])
{
    out[0] = 0x08;
    out[1] = 0x00;
    out[2] = 0x2b;
    out[3] = 0x11;
    out[4] = 0x22;
    out[5] = last;
}

static int is_zero6(const uint8_t *p)
{
    static const uint8_t z[6] = {0, 0, 0, 0, 0, 0};
    return memcmp(p, z, 6) == 0;
}

/* A whole SCSD peer world: configuration queue + the one local port's PDT + the
 * peer table, initialized the way main() initializes them. */
struct world {
    struct scs_config cfg;
    struct scs_pdt pdt;
    struct peer_state peers[OVMX_MAX_PEERS];
};

static void world_init(struct world *w)
{
    memset(w, 0, sizeof(*w));
    scs_config_init(&w->cfg);
    scs_pdt_init(&w->pdt, SCS_PORT_TYPE_ETHERNET, SCA_FRAME_MAX);
}

/* OVMX's own identities, held constant across every comparison below so that any
 * byte difference between two captured frames is attributable to the peer state. */
static const uint8_t our_hw_mac[6] = {0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc};
static const uint8_t our_logical[6] = {0xaa, 0x00, 0x04, 0x00, 0x41, 0x05};

/*
 * Drive the REAL scsd.c joiner CONNECT-REQUEST sender for a freshly discovered
 * peer with the given port address and learned System Address, and copy out the
 * frame scsd.c handed to the transmit path.
 */
static size_t capture_joiner_request(const uint8_t mac[6], const uint8_t sysid[6],
                                     uint8_t *out, uint8_t out_dst[6])
{
    struct world w;
    world_init(&w);
    struct peer_state *ps = peer_find_or_add(&w.cfg, &w.pdt, w.peers, mac);
    if (ps == NULL) {
        return 0;
    }
    ps_learn_sys_addr(&w.cfg, ps, sysid);
    scsd_test_frames = 0;
    scsd_test_last_len = 0;
    /* vms-398: these address-plumbing captures NAME the circuit (ps->pb), which
     * is exactly what the sender used before CONNECT gained the p. 2-47
     * selection step -- so what they assert is unchanged. The selection path
     * (named_vc == NULL, the one the daemon uses) is covered separately by
     * test_connect_selects_the_open_vc_via_config_sys(). */
    if (!send_joiner_connect_request(7 /* fd never touched under the seam */, 1, &w.cfg, ps,
                                     ps->pb, our_hw_mac, our_logical)) {
        return 0;
    }
    memcpy(out, scsd_test_last_frame, scsd_test_last_len);
    memcpy(out_dst, scsd_test_last_dst, 6);
    return scsd_test_last_len;
}

/* Positions at which two equal-length frames differ. Returns the count. */
static unsigned diff_positions(const uint8_t *a, const uint8_t *b, size_t n,
                               size_t *first, size_t *last)
{
    unsigned count = 0;
    *first = 0;
    *last = 0;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            if (count == 0) {
                *first = i;
            }
            *last = i;
            count++;
        }
    }
    return count;
}

/* ------------------------------------------------------------------------- */

/*
 * The bytes ps_sys_addr() feeds are the SCA peer-logical field. Change ONLY the
 * System Address the peer advertised and exactly six contiguous bytes of the
 * emitted frame change, to exactly those values. This is the wire-equivalence
 * property the refactor has to keep, asserted against the real sender rather
 * than against a hard-coded offset.
 */
static size_t peer_logical_offset = 0;

static void test_learned_system_address_is_the_peer_logical_field(void)
{
    const uint8_t *mac = port_x;
    const uint8_t *sid_a = addr_x;
    const uint8_t *sid_b = addr_y;
    uint8_t fa[SCA_FRAME_MAX], fb[SCA_FRAME_MAX];
    uint8_t dst_a[6], dst_b[6];

    size_t la = capture_joiner_request(mac, sid_a, fa, dst_a);
    size_t lb = capture_joiner_request(mac, sid_b, fb, dst_b);

    CHECK(la == (size_t)SCS_CONNECT_FRAME_LEN, "joiner CONNECT-REQUEST length %zu", la);
    CHECK(la == lb, "frame length changed with the system address (%zu vs %zu)", la, lb);
    if (la == 0 || la != lb) {
        return;
    }
    CHECK(memcmp(dst_a, mac, 6) == 0, "frame was not addressed to the peer's port");

    size_t first = 0, last = 0;
    unsigned n = diff_positions(fa, fb, la, &first, &last);
    CHECK(n == 6, "changing the learned system address changed %u bytes, expected 6", n);
    CHECK(last == first + 5, "the changed bytes are not contiguous (%zu..%zu)", first, last);
    if (n != 6 || last != first + 5) {
        return;
    }
    CHECK(memcmp(fa + first, sid_a, 6) == 0, "peer-logical is not the learned address");
    CHECK(memcmp(fb + first, sid_b, 6) == 0, "peer-logical did not follow the re-learn");
    peer_logical_offset = first;
}

/*
 * The peer's Ethernet PORT address (PB, p. 2-12) and its SCS SYSTEM address (SB,
 * p. 2-16) are different things and must stay different on the wire: changing the
 * port address moves only the destination, never the peer-logical field.
 */
static void test_port_address_and_system_address_stay_distinct(void)
{
    const uint8_t *mac_a = port_x;
    const uint8_t *mac_b = port_y;
    const uint8_t *sid = addr_x;
    uint8_t fa[SCA_FRAME_MAX], fb[SCA_FRAME_MAX];
    uint8_t dst_a[6], dst_b[6];

    size_t la = capture_joiner_request(mac_a, sid, fa, dst_a);
    size_t lb = capture_joiner_request(mac_b, sid, fb, dst_b);
    CHECK(la == lb && la > 0, "capture failed (%zu, %zu)", la, lb);
    if (la == 0 || la != lb) {
        return;
    }
    CHECK(memcmp(dst_a, mac_a, 6) == 0 && memcmp(dst_b, mac_b, 6) == 0,
          "the transmit destination did not follow the peer's port address");

    size_t first = 0, last = 0;
    unsigned n = diff_positions(fa, fb, la, &first, &last);
    CHECK(n == 6 && first == 0 && last == 5,
          "changing the port address changed %u bytes at %zu..%zu, expected the"
          " 6 destination-MAC bytes", n, first, last);
    if (peer_logical_offset > 0) {
        CHECK(memcmp(fa + peer_logical_offset, fb + peer_logical_offset, 6) == 0,
              "the port address leaked into the peer-logical field");
    }
}

/* A peer whose System Address is not learned yet emits zeros there -- byte for
 * byte what the zero-initialized per-peer field it replaced produced. */
static void test_undiscovered_system_address_is_zero(void)
{
    struct world w;
    world_init(&w);
    uint8_t mac[6];
    mac_of(0x33, mac);
    struct peer_state *ps = peer_find_or_add(&w.cfg, &w.pdt, w.peers, mac);
    CHECK(ps != NULL, "first contact did not allocate a peer slot");
    if (ps == NULL) {
        return;
    }
    CHECK(ps->pb != NULL, "first contact did not create a Path Block");
    CHECK(ps->pb != NULL && ps->pb->vc_state == SCS_VC_CLOSED, "new PB is not CLOSED");
    CHECK(is_zero6(ps_sys_addr(ps)), "unlearned system address is not zero");
    CHECK(memcmp(ps_port_addr(ps), mac, 6) == 0, "port address not recorded");

    scsd_test_frames = 0;
    CHECK(send_joiner_connect_request(7, 1, &w.cfg, ps, ps->pb, our_hw_mac, our_logical) == 1,
          "sender refused to build a frame");
    if (peer_logical_offset > 0 && scsd_test_frames == 1) {
        CHECK(is_zero6(scsd_test_last_frame + peer_logical_offset),
              "unlearned peer emitted a non-zero peer-logical");
    }
}

/* Peer slots stay one-per-port-address, and a full table still answers "no room"
 * the way the pre-refactor lookup did. */
static void test_peer_slot_identity_and_capacity(void)
{
    struct world w;
    world_init(&w);
    uint8_t mac[6];
    struct peer_state *slots[OVMX_MAX_PEERS];

    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        mac_of((uint8_t)(0x40 + i), mac);
        slots[i] = peer_find_or_add(&w.cfg, &w.pdt, w.peers, mac);
        CHECK(slots[i] != NULL, "peer %d was refused a slot", i);
        CHECK(peer_find_or_add(&w.cfg, &w.pdt, w.peers, mac) == slots[i],
              "a repeat contact from peer %d allocated a second slot", i);
    }
    for (int i = 1; i < OVMX_MAX_PEERS; i++) {
        CHECK(slots[i] != slots[0], "distinct ports collapsed onto one slot");
    }
    mac_of(0xfe, mac);
    CHECK(peer_find_or_add(&w.cfg, &w.pdt, w.peers, mac) == NULL,
          "a full peer table accepted one more peer");
    CHECK(scs_pdt_formative_count(&w.pdt) == (unsigned)OVMX_MAX_PEERS,
          "formative PB count does not match the peers discovered");
}

/*
 * RISK (a). Two peer ports that resolve to ONE SCS System ID share a System Block
 * after the p. 2-21 open transition, so ps_sys_addr() returns shared state where
 * it used to return a per-peer copy.
 *
 * What is asserted, and why it is not a regression at the moment of sharing: the
 * two Path Blocks can only share an SB because they carried the SAME System ID,
 * so both peers keep emitting exactly the bytes the pre-refactor per-peer copies
 * held. What IS new is the coupling afterwards -- a re-learn on either port now
 * moves the address for both. That is the architected behaviour (SCA keeps ONE
 * System Block per node, p. 2-16) and it is asserted here so it is a decision on
 * the record rather than an accident, together with its blast radius on the wire.
 *
 * REACHABILITY: this needs a peer node presenting two Ethernet ports with one
 * SCS System ID. SCSD can form that state (peer_find_or_add keys on MAC) but the
 * reference lab has never presented such a node, so the daemon has only ever
 * taken the SCS_OPEN_NEW_SB path.
 */
static void test_shared_sb_aliases_the_peer_logical(void)
{
    struct world w;
    world_init(&w);
    uint8_t mac_a[6], mac_b[6], sid[6], sid2[6];
    mac_of(0x51, mac_a);
    mac_of(0x52, mac_b);
    sysid_of(1329, sid);
    sysid_of(1400, sid2);

    struct peer_state *pa = peer_find_or_add(&w.cfg, &w.pdt, w.peers, mac_a);
    struct peer_state *pb = peer_find_or_add(&w.cfg, &w.pdt, w.peers, mac_b);
    CHECK(pa != NULL && pb != NULL, "two-port peer did not get two slots");
    if (pa == NULL || pb == NULL) {
        return;
    }
    ps_learn_sys_addr(&w.cfg, pa, sid);
    ps_learn_sys_addr(&w.cfg, pb, sid);

    /* Before the open transition each port still has its own formative SB. */
    CHECK(pa->pb->sb != pb->pb->sb, "formative System Blocks were shared too early");
    CHECK(memcmp(ps_sys_addr(pa), sid, 6) == 0 && memcmp(ps_sys_addr(pb), sid, 6) == 0,
          "learned system address not visible on both ports");

    CHECK(scs_pb_open(&w.cfg, pa->pb) == SCS_OPEN_NEW_SB, "first circuit was not NEW_SB");
    CHECK(scs_pb_open(&w.cfg, pb->pb) == SCS_OPEN_EXISTING_SB,
          "second circuit to the same node was not EXISTING_SB");
    CHECK(pa->pb->sb == pb->pb->sb, "the two circuits did not converge on one System Block");
    CHECK(scs_config_sb_count(&w.cfg) == 1, "one node produced more than one System Block");

    /* The bytes on the wire at the moment of sharing are unchanged for both. */
    CHECK(memcmp(ps_sys_addr(pa), sid, 6) == 0, "port A peer-logical changed on open");
    CHECK(memcmp(ps_sys_addr(pb), sid, 6) == 0, "port B peer-logical changed on open");

    /* THE DELTA, on the record: a re-learn on one port now moves both, because
     * both describe the same node. Pre-vms-7be only port B would have moved. */
    ps_learn_sys_addr(&w.cfg, pb, sid2);
    CHECK(memcmp(ps_sys_addr(pb), sid2, 6) == 0, "re-learn did not take on port B");
    CHECK(memcmp(ps_sys_addr(pa), sid2, 6) == 0,
          "shared System Block did not propagate to port A -- the aliasing"
          " documented in scs_config.h no longer holds; re-read that note");

    /* And it does reach the wire from port A. */
    scsd_test_frames = 0;
    CHECK(send_joiner_connect_request(7, 1, &w.cfg, pa, pa->pb, our_hw_mac, our_logical) == 1,
          "sender refused to build a frame for port A");
    if (peer_logical_offset > 0 && scsd_test_frames == 1) {
        CHECK(memcmp(scsd_test_last_frame + peer_logical_offset, sid2, 6) == 0,
              "port A's emitted peer-logical did not follow the shared System Block");
    }
}

/*
 * RISK (b). scs_pb_learn_system_addr() returns NULL when the SB pool is empty,
 * where the pre-refactor memcpy always stored. Two things are asserted:
 *   1. it is UNREACHABLE at the shipped pool sizes -- a full peer table plus the
 *      local node's own SB (the exact allocation SCSD's main() performs) leaves
 *      every peer with a correctly recorded address;
 *   2. if it were reachable it is HONEST, not silent -- the frame degrades to a
 *      zero peer-logical and SCSD logs SCSD-E-NOSB (CLAUDE.md rule 9 / INV-6).
 */
static void test_sb_exhaustion_is_visible_and_unreachable(void)
{
    /* 1. The daemon's own worst case: local SB + OVMX_MAX_PEERS peers. */
    struct world w;
    world_init(&w);
    struct scs_sb_info self;
    memset(&self, 0, sizeof(self));
    sysid_of(1029, self.system_id);
    self.node_name = "OVMX";
    CHECK(scs_config_insert_sb(&w.cfg, &self) != NULL, "local node's own SB was refused");

    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        uint8_t mac[6], sid[6];
        mac_of((uint8_t)(0x60 + i), mac);
        sysid_of((uint16_t)(1300 + i), sid);
        struct peer_state *ps = peer_find_or_add(&w.cfg, &w.pdt, w.peers, mac);
        CHECK(ps != NULL, "peer %d refused at the shipped pool sizes", i);
        if (ps == NULL) {
            continue;
        }
        ps_learn_sys_addr(&w.cfg, ps, sid);
        CHECK(memcmp(ps_sys_addr(ps), sid, 6) == 0,
              "peer %d lost its system address at the shipped pool sizes", i);
    }

    /* 2. The degraded path, forced by exhausting the SB pool outright. */
    struct world x;
    world_init(&x);
    unsigned inserted = 0;
    for (unsigned i = 0; i < SCS_CONFIG_MAX_SB + 4; i++) {
        struct scs_sb_info info;
        memset(&info, 0, sizeof(info));
        sysid_of((uint16_t)(2000 + i), info.system_id);
        if (scs_config_insert_sb(&x.cfg, &info) == NULL) {
            break;
        }
        inserted++;
    }
    CHECK(inserted == SCS_CONFIG_MAX_SB, "SB pool held %u SBs, expected %u", inserted,
          (unsigned)SCS_CONFIG_MAX_SB);

    uint8_t mac[6], sid[6];
    mac_of(0x70, mac);
    sysid_of(1329, sid);
    struct peer_state *ps = peer_find_or_add(&x.cfg, &x.pdt, x.peers, mac);
    CHECK(ps != NULL, "peer refused a slot with the SB pool full");
    if (ps == NULL) {
        return;
    }
    CHECK(scs_pb_learn_system_addr(&x.cfg, ps->pb, sid) == NULL,
          "the SB pool was not actually exhausted -- this test proves nothing");

    /* The failure must be LOUD, so capture SCSD's stderr and assert the message
     * rather than eyeballing it. */
    char logbuf[512];
    memset(logbuf, 0, sizeof(logbuf));
    FILE *cap = tmpfile();
    CHECK(cap != NULL, "could not open a capture file for stderr");
    if (cap != NULL) {
        int saved_fd = dup(STDERR_FILENO);
        fflush(stderr);
        dup2(fileno(cap), STDERR_FILENO);
        ps_learn_sys_addr(&x.cfg, ps, sid);
        fflush(stderr);
        dup2(saved_fd, STDERR_FILENO);
        close(saved_fd);
        rewind(cap);
        size_t got = fread(logbuf, 1, sizeof(logbuf) - 1, cap);
        logbuf[got] = '\0';
        fclose(cap);
    } else {
        ps_learn_sys_addr(&x.cfg, ps, sid);
    }
    CHECK(strstr(logbuf, "SCSD-E-NOSB") != NULL,
          "a peer whose system address could not be recorded failed SILENTLY"
          " (CLAUDE.md rule 9 / INV-6); stderr was: '%s'", logbuf);
    CHECK(is_zero6(ps_sys_addr(ps)),
          "exhausted SB pool still produced a system address");

    scsd_test_frames = 0;
    CHECK(send_joiner_connect_request(7, 1, &x.cfg, ps, ps->pb, our_hw_mac, our_logical) == 1,
          "sender refused to build a frame");
    if (peer_logical_offset > 0 && scsd_test_frames == 1) {
        CHECK(is_zero6(scsd_test_last_frame + peer_logical_offset),
              "degraded peer did not emit a zero peer-logical");
        CHECK(memcmp(scsd_test_last_dst, mac, 6) == 0,
              "degraded peer lost its destination port address too");
    }
}

/*
 * vms-17f: THE PEER SLOT IS THE THING THAT PINNED THE REJOIN, and this is the
 * mechanism at slot level, without any frames.
 *
 * Until vms-17f peer_find_or_add() matched on port address and only ever
 * ALLOCATED, so a node that left and came back re-entered its stale slot on its
 * still-OPEN Path Block and the second open took scs_pb_open's already-open
 * early return. Releasing the slot is what gives the returning node a second,
 * FORMATIVE circuit for the p. 2-21 Note to fire on. The full sequence, driven
 * by captured frames through the daemon's own receive dispatch, is
 * test_rejoin_reaches_the_p221_refresh() further down; this case isolates the
 * slot behaviour so a failure there can be told apart from a failure here.
 */
static void test_released_peer_slot_gives_a_returning_node_a_new_circuit(void)
{
    struct world w;
    world_init(&w);
    uint8_t mac[6], sid[6];
    mac_of(0x81, mac);
    sysid_of(1329, sid);

    struct peer_state *ps = peer_find_or_add(&w.cfg, &w.pdt, w.peers, mac);
    CHECK(ps != NULL, "peer refused a slot");
    if (ps == NULL) {
        return;
    }
    ps_learn_sys_addr(&w.cfg, ps, sid);
    struct scs_pb *first_pb = ps->pb;
    CHECK(scs_pb_open(&w.cfg, ps->pb) == SCS_OPEN_NEW_SB, "first join was not NEW_SB");
    ps->start_acked = 1; /* as scsd_vc_settle latches it when the circuit opens */

    /* THE OLD BEHAVIOUR, still exactly reachable: without a departure the lookup
     * hands back the SAME slot and the SAME already-open Path Block. */
    struct peer_state *again = peer_find_or_add(&w.cfg, &w.pdt, w.peers, mac);
    CHECK(again == ps, "an undeparted peer did not resolve to its own slot");
    CHECK(again != NULL && again->pb == first_pb, "an undeparted peer got a new Path Block");
    CHECK(scs_pb_open(&w.cfg, ps->pb) == SCS_OPEN_EXISTING_SB,
          "a second open of the SAME open PB no longer takes the early return");

    /* THE DEPARTURE. The Path Block goes, the slot goes, the System Block stays
     * (p. 2-17) -- and start_acked goes with the slot, which is what lets
     * formation run again for the returning node. */
    CHECK(scs_pb_depart(NULL, &w.cfg, ps->pb, NULL) == SCS_PB_CLOSE_OK,
          "the departure did not close the Path Block");
    memset(ps, 0, sizeof(*ps));
    CHECK(scs_config_sb_count(&w.cfg) == 1, "the System Block did not survive the departure");

    struct peer_state *back = peer_find_or_add(&w.cfg, &w.pdt, w.peers, mac);
    CHECK(back != NULL, "the returning peer was refused a slot");
    if (back == NULL) {
        return;
    }
    CHECK(back->start_acked == 0, "the returning peer inherited start_acked from its"
                                  " previous incarnation -- formation would not re-run");
    CHECK(back->pb != first_pb || back->pb->on_pdt == 1,
          "the returning peer did not get a FORMATIVE Path Block");
    CHECK(scs_pdt_formative_count(&w.pdt) == 1,
          "the returning peer's Path Block is not queued to the PDT (p. 2-20)");
    ps_learn_sys_addr(&w.cfg, back, sid);
    CHECK(scs_pb_open(&w.cfg, back->pb) == SCS_OPEN_EXISTING_REFRESHED,
          "the returning peer's open did not take the p. 2-21 REFRESH");
    CHECK(scs_config_sb_count(&w.cfg) == 1, "rejoin duplicated the System Block");
}

/*
 * =====================================================================
 * vms-4071 -- the DAEMON's use of the VC formation state machine.
 *
 * test_scs_vc.c proves the machine. This proves scsd.c's translation of the
 * machine's actions into 0x41 frames, through the SAME transmit seam and the
 * SAME builders the daemon uses -- i.e. the bytes asserted here are the bytes
 * scsd.c would have handed to sendto().
 * =====================================================================
 */

static uint16_t le16_at(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* The daemon-side context, built the way main() builds it. */
static struct scsd_vc_ctx vc_test_ctx(struct world *w)
{
    struct scsd_vc_ctx ctx;
    ctx.sock = 7; /* never touched under the seam */
    ctx.ifindex = 1;
    ctx.hw_mac = our_hw_mac;
    ctx.src_logical = our_logical;
    ctx.scssystemid = 1030;
    ctx.node_name = "OVMX";
    ctx.cfg = &w->cfg;
    return ctx;
}

/*
 * Each SEND_* action must produce the frame class the NISCA mapping claims:
 * START -> 106-byte config-round 0, STACK -> 106-byte config-round 1,
 * ACK -> the 46-byte round-2 frame. Offsets are absolute (payload + 14).
 */
static void test_vc_actions_emit_the_right_config_rounds(void)
{
    struct world w;
    world_init(&w);
    uint8_t peer_mac[6];
    mac_of(0x71, peer_mac);
    uint8_t sysid[6];
    sysid_of(1025, sysid);
    struct peer_state *ps = peer_find_or_add(&w.cfg, &w.pdt, w.peers, peer_mac);
    CHECK(ps != NULL, "no peer slot for the VC action test");
    if (ps == NULL) {
        return;
    }
    ps_learn_sys_addr(&w.cfg, ps, sysid);
    scs_vc_init(&ps->vc);
    struct scsd_vc_ctx ctx = vc_test_ctx(&w);

    scsd_test_frames = 0;
    CHECK(scsd_vc_emit(&ctx, ps, SCS_VC_ACT_SEND_START) == 1, "SEND_START emitted nothing");
    CHECK(scsd_test_last_len == SCS_START_FRAME_LEN,
          "SEND_START frame is %zu bytes, expected the 120-byte START class",
          scsd_test_last_len);
    CHECK(scsd_test_last_frame[30] == SCS_START_OPCODE, "SEND_START opcode is not 0x41");
    CHECK(le16_at(scsd_test_last_frame + 14 + 44) == 0,
          "SEND_START carries config-round %u, expected 0",
          le16_at(scsd_test_last_frame + 14 + 44));
    CHECK(memcmp(scsd_test_last_dst, peer_mac, 6) == 0, "SEND_START went to the wrong MAC");
    uint8_t start_frame[SCS_START_FRAME_LEN];
    memcpy(start_frame, scsd_test_last_frame, SCS_START_FRAME_LEN);

    CHECK(scsd_vc_emit(&ctx, ps, SCS_VC_ACT_SEND_STACK) == 1, "SEND_STACK emitted nothing");
    CHECK(scsd_test_last_len == SCS_START_FRAME_LEN,
          "SEND_STACK is not the 106-byte identity-bearing class -- p. 2-12 says a"
          " STACK re-supplies the node description");
    CHECK(le16_at(scsd_test_last_frame + 14 + 44) == 1,
          "SEND_STACK carries config-round %u, expected 1",
          le16_at(scsd_test_last_frame + 14 + 44));
    /* The STACK differs from the START in the config-round field and nothing
     * else: it really is the same identity body sent again. */
    size_t first = 0, last = 0;
    unsigned ndiff = diff_positions(start_frame, scsd_test_last_frame,
                                    SCS_START_FRAME_LEN, &first, &last);
    CHECK(ndiff == 1 && first == 14 + 44,
          "START vs STACK differ in %u byte(s) starting at %zu; expected exactly the"
          " config-round field at %d", ndiff, first, 14 + 44);

    CHECK(scsd_vc_emit(&ctx, ps, SCS_VC_ACT_SEND_ACK) == 1, "SEND_ACK emitted nothing");
    CHECK(scsd_test_last_len == SCS_START_ACK_FRAME_LEN,
          "SEND_ACK is not the 46-byte no-identity ACK class");
    CHECK(le16_at(scsd_test_last_frame + 14 + 44) == SCS_START_ACK_ROUND,
          "SEND_ACK carries config-round %u, expected 2",
          le16_at(scsd_test_last_frame + 14 + 44));

    /* Non-emitting actions must put nothing on the wire. */
    unsigned before = scsd_test_frames;
    CHECK(scsd_vc_emit(&ctx, ps, SCS_VC_ACT_NONE) == 0, "ACT_NONE emitted a frame");
    CHECK(scsd_vc_emit(&ctx, ps, SCS_VC_ACT_ABANDON) == 0, "ACT_ABANDON emitted a frame");
    CHECK(scsd_test_frames == before, "a non-emitting action reached the transmit path");
}

/*
 * THE HAPPY PATH, AND THE ORDERING IT PINS.
 *
 * OVMX emits three frames -- round-0 START, round-1 STACK, round-2 ACK -- and
 * the Path Block ends OPEN. What this test exists to pin is not the three
 * frames (that was never in doubt) but WHERE THE THIRD ONE FALLS RELATIVE TO
 * THE PEER'S FRAMES, because that is the part vms-4071 could silently change:
 *
 *   PRESERVED (this test, the shipped default):
 *       OVMX r0, OVMX r1, peer r1, peer r2, OVMX r2
 *   OPT-IN via OVMX_VC_EARLY_ACK=1 (test_vc_early_ack_is_opt_in below):
 *       OVMX r0, OVMX r1, peer r1, OVMX r2, peer r2
 *
 * The bytes of all three OVMX frames are identical either way; only the
 * interleaving differs. The default is the pre-vms-4071 interleaving -- so the
 * assertion below that the ack has NOT gone out after the peer's round-1 STACK
 * is the regression guard, and it fails if the early ordering ever becomes the
 * default.
 *
 * This test reads the AMBIENT environment on purpose -- it does not clear
 * OVMX_VC_EARLY_ACK for itself. Running the binary with OVMX_VC_EARLY_ACK=1
 * therefore reds it, which is the intended report: the shipped wire ordering is
 * no longer the preserved one. (Measured: 4 failures under
 * `OVMX_VC_EARLY_ACK=1 ./test_scsd_wire`, 0 with the switch unset.)
 */
static void test_vc_happy_path_frame_sequence(void)
{
    struct world w;
    world_init(&w);
    uint8_t peer_mac[6];
    mac_of(0x72, peer_mac);
    uint8_t sysid[6];
    sysid_of(1025, sysid);
    struct peer_state *ps = peer_find_or_add(&w.cfg, &w.pdt, w.peers, peer_mac);
    CHECK(ps != NULL, "no peer slot for the happy-path test");
    if (ps == NULL) {
        return;
    }
    ps_learn_sys_addr(&w.cfg, ps, sysid);
    scs_vc_init(&ps->vc);
    struct scsd_vc_ctx ctx = vc_test_ctx(&w);
    long acks = 0;
    scsd_test_frames = 0;

    /* The shipped default must be the preserved ordering, with nothing in the
     * environment required to get it. */
    CHECK(scs_vc_early_ack_enabled() == 0,
          "OVMX_VC_EARLY_ACK defaults ON -- the fresh-join interleaving is not preserved");

    /* CLOSED -> START SENT, round-0 START out. */
    CHECK(scs_vc_fsm_send_start(ps->pb, 0) == SCS_VC_ACT_SEND_START, "no START action");
    CHECK(scsd_vc_emit(&ctx, ps, SCS_VC_ACT_SEND_START) == 1, "round-0 START not sent");
    CHECK(le16_at(scsd_test_last_frame + 14 + 44) == 0, "first frame is not round 0");

    /* Peer round-0 START -> round-1 STACK out, START RECEIVED. */
    enum scs_vc_action act = scs_vc_fsm_recv(ps->pb, scs_vc_classify_round(0, 0), 10);
    CHECK(act == SCS_VC_ACT_SEND_STACK, "peer round-0 did not produce a STACK");
    CHECK(scsd_vc_emit(&ctx, ps, act) == 1, "round-1 STACK not sent");
    CHECK(le16_at(scsd_test_last_frame + 14 + 44) == 1, "second frame is not round 1");
    CHECK(ps->pb->vc_state == SCS_VC_START_RECEIVED, "VC is not START RECEIVED");
    scsd_vc_settle(&ctx, ps, act, /*peer_round2_seen=*/0, &acks);
    CHECK(acks == 0, "the round-2 ack went out before the circuit was OPEN");

    /* Peer round-1 STACK -> the circuit OPENS. The SCA table (p. 2-14) says to
     * issue an ACK here, and the machine duly returns SEND_ACK -- but the
     * DEFAULT NISCA ordering holds it back, because pre-vms-4071 OVMX did not
     * put its round-2 frame out until the peer's round-2 arrived. THIS IS THE
     * REGRESSION GUARD: if the early ordering ever becomes the default, `acks`
     * is 1 here and this assertion reds. */
    enum scs_vc_event ev = scs_vc_classify_round(0, 1);
    CHECK(scsd_vc_peer_round2(ev) == 0, "the daemon read a round-1 STACK as the peer's round-2");
    act = scs_vc_fsm_recv(ps->pb, ev, 20);
    CHECK(act == SCS_VC_ACT_SEND_ACK, "peer round-1 did not produce an ACK action");
    CHECK(ps->pb->vc_state == SCS_VC_OPEN, "VC is not OPEN after the peer's STACK");
    unsigned frames_before_ack = scsd_test_frames;
    scsd_vc_settle(&ctx, ps, act, scsd_vc_peer_round2(ev), &acks);
    CHECK(acks == 0,
          "the round-2 ack was emitted on the OPEN transition -- that is the OPT-IN"
          " OVMX_VC_EARLY_ACK ordering, not the preserved default");
    CHECK(scsd_test_frames == frames_before_ack && frames_before_ack == 2,
          "%u frames after the peer's STACK, expected the 2 OVMX has sent so far",
          scsd_test_frames);
    CHECK(ps->start_acked == 0, "start_acked latched before the ack was sent");

    /* Peer round-2 ack -> OVMX's round-2 ack out, exactly once. This is the
     * pre-vms-4071 trigger, and the third argument is how the daemon spells it
     * (scsd.c passes `vc_ev == SCS_VC_EV_ACK`). */
    ev = scs_vc_classify_round(1, 2);
    CHECK(scsd_vc_peer_round2(ev) == 1,
          "the daemon does not read the peer's 46-byte round-2 frame as its ack trigger");
    act = scs_vc_fsm_recv(ps->pb, ev, 30);
    CHECK(act == SCS_VC_ACT_NONE, "a peer ACK on an already-OPEN circuit is discarded (p. 2-12)");
    scsd_vc_settle(&ctx, ps, act, scsd_vc_peer_round2(ev), &acks);
    CHECK(acks == 1, "the round-2 ack was not sent when the peer's round-2 arrived");
    CHECK(scsd_test_last_len == SCS_START_ACK_FRAME_LEN, "third frame is not the 46-byte ack");
    CHECK(ps->pb->vc_state == SCS_VC_OPEN, "VC is not OPEN");
    CHECK(ps->start_acked == 1, "start_acked was not latched");
    CHECK(scsd_test_frames == 3, "the dialogue emitted %u frames, expected exactly 3",
          scsd_test_frames);

    /* The p. 2-21 open transition ran, and it is still the only one SCSD reaches. */
    CHECK(scs_config_sb_count(&w.cfg) == 1, "the peer's System Block was not queued");
    CHECK(scs_sb_pb_count(ps->pb->sb) == 1, "the Path Block did not join its System Block");
    CHECK(scs_pdt_formative_count(&w.pdt) == 0, "the Path Block is still formative");
    CHECK(ps->vc.seq.send_seq == 1 && ps->vc.seq.recv_seq == 0,
          "the SCS VC was not reset at START completion (vms-246)");

    /* A duplicate peer round-2 must add no frame. */
    unsigned before = scsd_test_frames;
    act = scs_vc_fsm_recv(ps->pb, scs_vc_classify_round(1, 2), 40);
    scsd_vc_settle(&ctx, ps, act, scsd_vc_peer_round2(scs_vc_classify_round(1, 2)), &acks);
    CHECK(scsd_test_frames == before && acks == 1,
          "a duplicate peer round-2 ack produced a duplicate OVMX ack");
}

/*
 * THE OPT-IN EARLY ACK. Same dialogue as above with OVMX_VC_EARLY_ACK=1: the
 * round-2 ack now goes out on the OPEN transition, one peer-frame earlier. Both
 * branches of the switch are exercised so that neither ordering can drift
 * unnoticed, and the OTHER env switch (OVMX_VC_NO_RETRY_LIMIT) is shown NOT to
 * reach this decision -- the two are independent.
 */
static void test_vc_early_ack_is_opt_in(void)
{
    struct world w;
    world_init(&w);
    uint8_t peer_mac[6];
    mac_of(0x76, peer_mac);
    uint8_t sysid[6];
    sysid_of(1025, sysid);
    struct peer_state *ps = peer_find_or_add(&w.cfg, &w.pdt, w.peers, peer_mac);
    if (ps == NULL) {
        CHECK(0, "no peer slot for the early-ack test");
        return;
    }
    ps_learn_sys_addr(&w.cfg, ps, sysid);
    scs_vc_init(&ps->vc);
    struct scsd_vc_ctx ctx = vc_test_ctx(&w);
    long acks = 0;

    /* Unlike the happy-path test above, THIS one is hermetic about its own
     * switch: it sets and clears OVMX_VC_EARLY_ACK itself, so it passes whether
     * or not the switch is set in the ambient environment. (The happy-path test
     * deliberately reads the ambient value -- that is how it pins the default.) */
    unsetenv(SCS_VC_EARLY_ACK_ENV);

    /* The retry-limit kill-switch must not move the ack ordering. */
    setenv(SCS_VC_NO_RETRY_LIMIT_ENV, "1", 1);
    CHECK(scs_vc_early_ack_enabled() == 0,
          "OVMX_VC_NO_RETRY_LIMIT=1 also turned on the early ack -- the switches are"
          " supposed to be independent");
    unsetenv(SCS_VC_NO_RETRY_LIMIT_ENV);

    /* Values other than exactly "1" must not enable it either. */
    setenv(SCS_VC_EARLY_ACK_ENV, "0", 1);
    CHECK(scs_vc_early_ack_enabled() == 0, "OVMX_VC_EARLY_ACK=0 enabled the early ack");
    setenv(SCS_VC_EARLY_ACK_ENV, "10", 1);
    CHECK(scs_vc_early_ack_enabled() == 0, "OVMX_VC_EARLY_ACK=10 enabled the early ack");

    setenv(SCS_VC_EARLY_ACK_ENV, "1", 1);
    CHECK(scs_vc_early_ack_enabled() == 1, "OVMX_VC_EARLY_ACK=1 did not enable the early ack");

    scsd_test_frames = 0;
    scs_vc_fsm_send_start(ps->pb, 0);
    scsd_vc_emit(&ctx, ps, SCS_VC_ACT_SEND_START);
    enum scs_vc_action act = scs_vc_fsm_recv(ps->pb, scs_vc_classify_round(0, 0), 10);
    scsd_vc_emit(&ctx, ps, act);
    scsd_vc_settle(&ctx, ps, act, /*peer_round2_seen=*/0, &acks);
    CHECK(acks == 0, "the early ack fired before the circuit was OPEN");

    /* The peer's round-1 STACK: with the switch ON, the ack goes out NOW. */
    act = scs_vc_fsm_recv(ps->pb, scs_vc_classify_round(0, 1), 20);
    CHECK(act == SCS_VC_ACT_SEND_ACK, "peer round-1 did not produce an ACK action");
    scsd_vc_settle(&ctx, ps, act, /*peer_round2_seen=*/0, &acks);
    CHECK(acks == 1, "OVMX_VC_EARLY_ACK=1 did not emit the ack on the OPEN transition");
    CHECK(scsd_test_frames == 3 && scsd_test_last_len == SCS_START_ACK_FRAME_LEN,
          "the early ack is not the third frame and the 46-byte ack class");
    CHECK(le16_at(scsd_test_last_frame + 14 + 44) == SCS_START_ACK_ROUND,
          "the early ack does not carry config-round 2");

    /* The peer's round-2 then adds nothing. */
    unsigned before = scsd_test_frames;
    act = scs_vc_fsm_recv(ps->pb, scs_vc_classify_round(1, 2), 30);
    scsd_vc_settle(&ctx, ps, act, /*peer_round2_seen=*/1, &acks);
    CHECK(scsd_test_frames == before && acks == 1,
          "the peer's round-2 produced a second ack under OVMX_VC_EARLY_ACK");

    unsetenv(SCS_VC_EARLY_ACK_ENV);
    CHECK(scs_vc_early_ack_enabled() == 0, "the early-ack switch did not clear");
}

/*
 * If the peer's round-1 STACK is lost and only its round-2 ack arrives, the SCA
 * table opens the circuit with NO emission (p. 2-14) -- but the NISCA dialogue
 * still owes the peer OVMX's round-2 frame, so scsd_vc_settle must send it.
 * The peer's round-2 frame is the DEFAULT ack trigger, so this works with no
 * env switch set.
 */
static void test_vc_open_on_bare_ack_still_sends_round_2(void)
{
    struct world w;
    world_init(&w);
    uint8_t peer_mac[6];
    mac_of(0x73, peer_mac);
    uint8_t sysid[6];
    sysid_of(1025, sysid);
    struct peer_state *ps = peer_find_or_add(&w.cfg, &w.pdt, w.peers, peer_mac);
    if (ps == NULL) {
        CHECK(0, "no peer slot for the bare-ack test");
        return;
    }
    ps_learn_sys_addr(&w.cfg, ps, sysid);
    scs_vc_init(&ps->vc);
    struct scsd_vc_ctx ctx = vc_test_ctx(&w);
    long acks = 0;

    scs_vc_fsm_send_start(ps->pb, 0);
    scsd_vc_emit(&ctx, ps, SCS_VC_ACT_SEND_START);
    enum scs_vc_action act = scs_vc_fsm_recv(ps->pb, SCS_VC_EV_START, 10);
    scsd_vc_emit(&ctx, ps, act);
    scsd_vc_settle(&ctx, ps, act, /*peer_round2_seen=*/0, &acks);

    scsd_test_frames = 0;
    act = scs_vc_fsm_recv(ps->pb, SCS_VC_EV_ACK, 20);
    CHECK(act == SCS_VC_ACT_NONE, "SCA says a bare ACK opens the circuit silently");
    scsd_vc_settle(&ctx, ps, act, /*peer_round2_seen=*/1, &acks);
    CHECK(ps->pb->vc_state == SCS_VC_OPEN, "the bare ACK did not open the circuit");
    CHECK(acks == 1 && scsd_test_frames == 1,
          "OVMX did not emit its round-2 ack after opening on a bare ACK");
    CHECK(scsd_test_last_len == SCS_START_ACK_FRAME_LEN, "the emitted frame is not the ack");
}

/*
 * The p. 2-16 implied ACK, through the daemon: a circuit packet from a peer in
 * START RECEIVED opens the circuit and settles it. Also pins WHICH opcodes
 * qualify -- feeding 0x41 or a HELLO here would open circuits mid-dialogue.
 */
static void test_vc_implied_ack_through_the_daemon(void)
{
    struct world w;
    world_init(&w);
    uint8_t peer_mac[6];
    mac_of(0x74, peer_mac);
    uint8_t sysid[6];
    sysid_of(1025, sysid);
    struct peer_state *ps = peer_find_or_add(&w.cfg, &w.pdt, w.peers, peer_mac);
    if (ps == NULL) {
        CHECK(0, "no peer slot for the implied-ACK test");
        return;
    }
    ps_learn_sys_addr(&w.cfg, ps, sysid);
    scs_vc_init(&ps->vc);
    struct scsd_vc_ctx ctx = vc_test_ctx(&w);
    long acks = 0;

    scs_vc_fsm_send_start(ps->pb, 0);
    scsd_vc_emit(&ctx, ps, SCS_VC_ACT_SEND_START);
    enum scs_vc_action act = scs_vc_fsm_recv(ps->pb, SCS_VC_EV_START, 10);
    scsd_vc_emit(&ctx, ps, act);
    scsd_vc_settle(&ctx, ps, act, /*peer_round2_seen=*/0, &acks);
    CHECK(ps->pb->vc_state == SCS_VC_START_RECEIVED, "precondition: START RECEIVED");

    /* The daemon's own lookup must find this PB by the peer's port address. */
    CHECK(scs_config_find_pb(&w.cfg, &w.pdt, peer_mac) == ps->pb,
          "the daemon cannot find the Path Block the implied-ACK hook needs");

    scsd_test_frames = 0;
    act = scs_vc_fsm_recv(ps->pb, SCS_VC_EV_OTHER, 20);
    /* peer_round2_seen=0, exactly as the daemon's implied-ACK site passes it: a
     * peer that is already sending circuit traffic will never send a round-2,
     * so waiting for one would deadlock. scsd_vc_ack_due() lets fsm.implied_acks
     * stand in. */
    scsd_vc_settle(&ctx, ps, act, /*peer_round2_seen=*/0, &acks);
    CHECK(ps->pb->vc_state == SCS_VC_OPEN, "the implied ACK did not open the circuit");
    CHECK(ps->pb->fsm.implied_acks == 1, "the implied ACK was not counted");
    CHECK(acks == 1 && scsd_test_frames == 1,
          "OVMX did not emit its round-2 ack after the implied ACK");
    CHECK(scs_vc_is_circuit_packet(SCS_START_OPCODE) == 0,
          "0x41 must NOT be treated as a circuit packet -- it would open forming circuits");
}

/*
 * The reissue/abandon path through the daemon: an unanswered START is reissued
 * on the wire, and when the retry limit is hit scsd_vc_settle re-arms the Path
 * Block instead of leaving the peer permanently unreachable.
 */
static void test_vc_reissue_and_abandon_through_the_daemon(void)
{
    struct world w;
    world_init(&w);
    uint8_t peer_mac[6];
    mac_of(0x75, peer_mac);
    uint8_t sysid[6];
    sysid_of(1025, sysid);
    struct peer_state *ps = peer_find_or_add(&w.cfg, &w.pdt, w.peers, peer_mac);
    if (ps == NULL) {
        CHECK(0, "no peer slot for the reissue test");
        return;
    }
    ps_learn_sys_addr(&w.cfg, ps, sysid);
    scs_vc_init(&ps->vc);
    struct scsd_vc_ctx ctx = vc_test_ctx(&w);
    long acks = 0;

    scs_vc_fsm_send_start(ps->pb, 0);
    scsd_vc_emit(&ctx, ps, SCS_VC_ACT_SEND_START);
    ps->start_replied = 1;

    /* Nothing is due before the timeout, and the reissue is byte-identical. */
    CHECK(scs_vc_fsm_timer_expired(ps->pb, SCS_VC_FORMATION_TIMEOUT_MS - 1,
                                   SCS_VC_FORMATION_TIMEOUT_MS) == 0,
          "the formation timer fired early");
    uint8_t original[SCS_START_FRAME_LEN];
    memcpy(original, scsd_test_last_frame, SCS_START_FRAME_LEN);

    scsd_test_frames = 0;
    enum scs_vc_action act = scs_vc_fsm_timeout(ps->pb, SCS_VC_FORMATION_TIMEOUT_MS, 3);
    CHECK(act == SCS_VC_ACT_SEND_START, "the expired timer did not reissue the START");
    CHECK(scsd_vc_emit(&ctx, ps, act) == 1, "the reissued START was not sent");
    CHECK(scsd_test_last_len == SCS_START_FRAME_LEN &&
              memcmp(scsd_test_last_frame, original, SCS_START_FRAME_LEN) == 0,
          "the reissued START is not byte-identical to the original");

    /* Drive it to the limit; the last expiry abandons. */
    act = scs_vc_fsm_timeout(ps->pb, 2 * SCS_VC_FORMATION_TIMEOUT_MS, 3);
    CHECK(act == SCS_VC_ACT_SEND_START, "second expiry did not reissue");
    act = scs_vc_fsm_timeout(ps->pb, 3 * SCS_VC_FORMATION_TIMEOUT_MS, 3);
    CHECK(act == SCS_VC_ACT_ABANDON, "the retry limit did not abandon formation");

    unsigned before = scsd_test_frames;
    scsd_vc_settle(&ctx, ps, act, /*peer_round2_seen=*/0, &acks);
    CHECK(scsd_test_frames == before && acks == 0,
          "abandoning formation put a frame on the wire");
    CHECK(ps->pb->vc_state == SCS_VC_CLOSED, "an abandoned circuit is not CLOSED");
    CHECK(ps->pb->fsm.abandoned == 0,
          "scsd_vc_settle left the Path Block permanently abandoned -- the peer could"
          " never form a circuit again");
    CHECK(ps->start_replied == 0, "the daemon's START-replied latch was not cleared");

    /* And the re-armed Path Block can start the dialogue over. */
    CHECK(scs_vc_fsm_send_start(ps->pb, 9999) == SCS_VC_ACT_SEND_START,
          "the re-armed Path Block cannot issue a fresh START");
}

/*
 * vms-398: the DAEMON's CONNECT really does select its virtual circuit from the
 * configuration database (p. 2-47), rather than reaching through the peer slot.
 *
 * Both of scsd.c's CONNECT-REQUEST call sites pass named_vc == NULL -- "the
 * caller did not name a circuit" -- so this drives the production sender the
 * production way and asserts three separate things:
 *   1. with an OPEN circuit, selection produces byte-for-byte the frame that
 *      naming the circuit produces (the wiring is wire-invisible);
 *   2. with no OPEN circuit for the node, NOTHING is transmitted and the refusal
 *      is LOGGED (CLAUDE.md rule 9 / INV-6: no silent fake success);
 *   3. selection follows the VIRTUAL-CIRCUIT STATE, not the peer slot: asked via
 *      a peer whose own Path Block is not OPEN, CONNECT sends over the node's
 *      OTHER, open Path Block.
 *
 * REACHABILITY of case 3: it needs the two-Ethernet-ports/one-System-ID node
 * that test_shared_sb_aliases_the_peer_logical() also documents; the reference
 * lab has never presented one, so this shape is formable by SCSD but unobserved.
 * Cases 1 and 2 are the shapes the daemon takes on every join.
 */
static void test_connect_selects_the_open_vc_via_config_sys(void)
{
    uint8_t mac[6], sid[6];
    mac_of(0x91, mac);
    sysid_of(1329, sid);

    /* --- 1. OPEN circuit: selecting == naming, byte for byte. --- */
    struct world w;
    world_init(&w);
    struct peer_state *ps = peer_find_or_add(&w.cfg, &w.pdt, w.peers, mac);
    CHECK(ps != NULL, "peer refused a slot");
    if (ps == NULL) {
        return;
    }
    ps_learn_sys_addr(&w.cfg, ps, sid);
    CHECK(scs_pb_open(&w.cfg, ps->pb) == SCS_OPEN_NEW_SB, "join was not NEW_SB");
    /* The daemon resets the SCS VC counters when START completes, immediately
     * before it opens the Path Block and sends this CONNECT-REQUEST; do the same
     * so the two captures below differ only in how the circuit was chosen. */
    scs_vc_reset_seq(&ps->vc);

    uint8_t named_frame[SCA_FRAME_MAX], named_dst[6];
    scsd_test_frames = 0;
    CHECK(send_joiner_connect_request(7, 1, &w.cfg, ps, ps->pb, our_hw_mac, our_logical) == 1,
          "sender refused a NAMED open circuit");
    size_t named_len = scsd_test_last_len;
    memcpy(named_frame, scsd_test_last_frame, named_len);
    memcpy(named_dst, scsd_test_last_dst, 6);

    scsd_test_frames = 0;
    CHECK(send_joiner_connect_request(7, 1, &w.cfg, ps, NULL, our_hw_mac, our_logical) == 1,
          "CONNECT with no named circuit found no OPEN virtual circuit for an open node");
    CHECK(scsd_test_frames == 1, "selection path transmitted %u frames, expected 1",
          scsd_test_frames);
    CHECK(scsd_test_last_len == named_len,
          "selected-circuit frame is %zu bytes, named-circuit frame is %zu",
          scsd_test_last_len, named_len);
    CHECK(named_len > 0 && scsd_test_last_len == named_len &&
              memcmp(scsd_test_last_frame, named_frame, named_len) == 0,
          "CONFIG_SYS selection changed the CONNECT-REQUEST bytes");
    CHECK(memcmp(scsd_test_last_dst, named_dst, 6) == 0 &&
              memcmp(scsd_test_last_dst, mac, 6) == 0,
          "selected circuit was not addressed to the peer's port");
    if (peer_logical_offset > 0) {
        CHECK(memcmp(scsd_test_last_frame + peer_logical_offset, sid, 6) == 0,
              "selected circuit did not carry the node's System Address");
    }

    /* --- 2. No OPEN circuit: refuse, transmit nothing, and SAY SO. --- */
    struct world f;
    world_init(&f);
    uint8_t mac2[6];
    mac_of(0x92, mac2);
    struct peer_state *forming = peer_find_or_add(&f.cfg, &f.pdt, f.peers, mac2);
    CHECK(forming != NULL, "forming peer refused a slot");
    if (forming == NULL) {
        return;
    }
    ps_learn_sys_addr(&f.cfg, forming, sid);
    scs_pb_set_vc_state(forming->pb, SCS_VC_START_SENT); /* still forming, never OPEN */

    char logbuf[512];
    logbuf[0] = '\0';
    int rc = 0;
    scsd_test_frames = 0;
    FILE *cap = tmpfile();
    if (cap != NULL) {
        int saved_fd = dup(STDERR_FILENO);
        fflush(stderr);
        dup2(fileno(cap), STDERR_FILENO);
        rc = send_joiner_connect_request(7, 1, &f.cfg, forming, NULL, our_hw_mac, our_logical);
        fflush(stderr);
        dup2(saved_fd, STDERR_FILENO);
        close(saved_fd);
        rewind(cap);
        size_t got = fread(logbuf, 1, sizeof(logbuf) - 1, cap);
        logbuf[got] = '\0';
        fclose(cap);
    } else {
        rc = send_joiner_connect_request(7, 1, &f.cfg, forming, NULL, our_hw_mac, our_logical);
    }
    CHECK(rc == 0, "CONNECT invented a circuit for a node with none OPEN");
    CHECK(scsd_test_frames == 0,
          "CONNECT transmitted %u frames with no OPEN virtual circuit", scsd_test_frames);
    CHECK(strstr(logbuf, "SCSD-E-NOVC") != NULL,
          "CONNECT refused SILENTLY (CLAUDE.md rule 9 / INV-6); stderr was: '%s'", logbuf);

    /* --- 3. Selection follows the circuit state, not the peer slot. --- */
    struct world t;
    world_init(&t);
    uint8_t mac_a[6], mac_b[6];
    mac_of(0xa1, mac_a);
    mac_of(0xa2, mac_b);
    struct peer_state *pa = peer_find_or_add(&t.cfg, &t.pdt, t.peers, mac_a);
    struct peer_state *pb2 = peer_find_or_add(&t.cfg, &t.pdt, t.peers, mac_b);
    CHECK(pa != NULL && pb2 != NULL, "two-port peer did not get two slots");
    if (pa == NULL || pb2 == NULL) {
        return;
    }
    ps_learn_sys_addr(&t.cfg, pa, sid);
    ps_learn_sys_addr(&t.cfg, pb2, sid);
    CHECK(scs_pb_open(&t.cfg, pa->pb) == SCS_OPEN_NEW_SB, "port A open was not NEW_SB");
    CHECK(scs_pb_open(&t.cfg, pb2->pb) == SCS_OPEN_EXISTING_SB, "port B did not join the SB");
    /* Port B is the HEAD of the node's Path Block queue (SBs queue at the head),
     * and its circuit has since gone down while its PB is still queued. */
    CHECK(pa->pb->sb != NULL && pa->pb->sb->pb_head == pb2->pb,
          "port B is not the head of the node's Path Block queue -- case 3 would"
          " pass without exercising the OPEN scan");
    scs_pb_set_vc_state(pb2->pb, SCS_VC_CLOSED);

    scsd_test_frames = 0;
    CHECK(send_joiner_connect_request(7, 1, &t.cfg, pb2, NULL, our_hw_mac, our_logical) == 1,
          "CONNECT found no OPEN circuit though port A's is open");
    CHECK(scsd_test_frames == 1, "CONNECT transmitted %u frames, expected 1",
          scsd_test_frames);
    CHECK(memcmp(scsd_test_last_dst, mac_a, 6) == 0,
          "CONNECT sent over the CLOSED circuit of the peer slot it was asked"
          " through instead of the node's OPEN one");
}

/* ==========================================================================
 * vms-dd5 -- THE CONNECTION STATE MACHINE, THROUGH THE REAL DAEMON.
 *
 * Two claims are made about wiring scsd.c to the CDL, and both are asserted
 * here over the production translation unit rather than by reading the diff
 * (the "structural diff-reading offered in place of a test" this epic has
 * already rejected once):
 *
 *   1. IT IS WIRE-INVISIBLE. The frame scsd.c hands the transmit path is
 *      byte-identical with the machine running and with OVMX_NO_CONN_FSM=1.
 *   2. THE KILL SWITCH GATES THE THING THAT EXISTS. With it set, no CDT is
 *      allocated, no transition is counted and no state is recorded -- and with
 *      it unset all three happen. Guardrail 23: the switch is RUN and the
 *      counter it gates is confirmed to move, in that order.
 * ========================================================================== */

/* Drive the real joiner sender once, from a fresh world and a fresh CDL. */
static size_t drive_joiner_once(uint8_t *out, uint8_t out_dst[6],
                                struct scs_cdt **cdt_out)
{
    struct world w;
    world_init(&w);
    scs_cdl_init(&scsd_cdl);
    scsd_cdl_ready = 1;
    conn_transitions = 0;
    conn_illegal_events = 0;
    conn_unemitted_actions = 0;

    static const uint8_t mac[6] = {0x08, 0x00, 0x2b, 0x11, 0x22, 0x33};
    static const uint8_t sysid[6] = {0xaa, 0x00, 0x04, 0x00, 0x31, 0x05};
    struct peer_state *ps = peer_find_or_add(&w.cfg, &w.pdt, w.peers, mac);
    if (ps == NULL) {
        return 0;
    }
    ps_learn_sys_addr(&w.cfg, ps, sysid);
    /* vms-398: the production call sites name only the NODE, so the circuit has
     * to be OPEN for CONFIG_SYS selection to find it. Open it the way the daemon
     * does, then call the sender exactly as the daemon calls it (named_vc NULL). */
    (void)scs_pb_open(&w.cfg, ps->pb);
    scsd_test_frames = 0;
    scsd_test_last_len = 0;
    if (!send_joiner_connect_request(7, 1, &w.cfg, ps, NULL, our_hw_mac, our_logical)) {
        return 0;
    }
    memcpy(out, scsd_test_last_frame, scsd_test_last_len);
    memcpy(out_dst, scsd_test_last_dst, 6);
    if (cdt_out != NULL) {
        *cdt_out = ps->cdt_joiner;
    }
    return scsd_test_last_len;
}

static void test_conn_fsm_does_not_change_the_wire(void)
{
    uint8_t on_frame[SCA_FRAME_MAX], off_frame[SCA_FRAME_MAX];
    uint8_t on_dst[6], off_dst[6];
    struct scs_cdt *on_cdt = NULL, *off_cdt = NULL;

    /* --- machine ON --- */
    CHECK(unsetenv("OVMX_NO_CONN_FSM") == 0, "unsetenv failed");
    size_t on_len = drive_joiner_once(on_frame, on_dst, &on_cdt);
    CHECK(on_len == SCS_CONNECT_FRAME_LEN, "joiner frame is %zu bytes, expected %d",
          on_len, SCS_CONNECT_FRAME_LEN);
    /* The counter the switch gates MUST have moved before anything is claimed
     * about what turning it off achieves (guardrail 23). */
    CHECK(conn_transitions == 1, "machine ON recorded %lu transitions, expected 1",
          conn_transitions);
    CHECK(conn_illegal_events == 0, "machine ON scored %lu illegal events",
          conn_illegal_events);
    CHECK(on_cdt != NULL, "no CDT was bound for the joiner connection");
    CHECK(on_cdt != NULL && on_cdt->local_conid == OVMX_JOINER_CONID,
          "the CDT did not claim the Con.ID that goes on the wire");
    CHECK(on_cdt != NULL && scs_conn_state_of(on_cdt) == SCS_CONN_CONNECT_SENT,
          "after sending a CONNECT_REQ the connection is %s, expected CONNECT SENT",
          on_cdt ? scs_conn_state_name(scs_conn_state_of(on_cdt)) : "(none)");
    CHECK(scs_cdl_in_use_count(&scsd_cdl) == 1, "expected exactly one CDT in the CDL");
    /* The CDT is reachable the p. 2-29 way, by CONID. */
    CHECK(scs_cdl_lookup(&scsd_cdl, OVMX_JOINER_CONID) == on_cdt,
          "the daemon's CDT is not reachable by its CONID through the CDL");

    /* --- machine OFF, same code path --- */
    CHECK(setenv("OVMX_NO_CONN_FSM", "1", 1) == 0, "setenv failed");
    size_t off_len = drive_joiner_once(off_frame, off_dst, &off_cdt);
    CHECK(off_len == on_len, "the frame changed length with the machine off (%zu vs %zu)",
          off_len, on_len);
    CHECK(conn_transitions == 0, "the kill switch did not stop transitions (%lu recorded)",
          conn_transitions);
    CHECK(off_cdt == NULL, "the kill switch did not stop CDT allocation");
    CHECK(scs_cdl_in_use_count(&scsd_cdl) == 0,
          "the kill switch left %u CDTs in the CDL", scs_cdl_in_use_count(&scsd_cdl));

    /* --- THE WIRE CLAIM --- */
    size_t first = 0, last = 0;
    unsigned d = diff_positions(on_frame, off_frame, on_len, &first, &last);
    CHECK(d == 0, "%u byte(s) of the CONNECT-REQUEST differ with the state machine"
                  " running (first at offset %zu) -- it is NOT wire-invisible",
          d, first);
    CHECK(memcmp(on_dst, off_dst, 6) == 0, "the destination MAC changed");

    CHECK(unsetenv("OVMX_NO_CONN_FSM") == 0, "unsetenv failed");
}

/*
 * A RETRANSMITTED CONNECT-REQUEST must not be scored an illegal event. scsd.c
 * re-sends this frame on a timer (vms-d94) and re-answers the member's repeats
 * (vms-c6d); if every repeat were illegal the run log would be unreadable and
 * the stuck diagnostic would be drowned out.
 */
static void test_joiner_retransmit_is_not_an_illegal_event(void)
{
    struct world w;
    world_init(&w);
    scs_cdl_init(&scsd_cdl);
    scsd_cdl_ready = 1;
    conn_transitions = 0;
    conn_illegal_events = 0;

    static const uint8_t mac[6] = {0x08, 0x00, 0x2b, 0x44, 0x55, 0x66};
    static const uint8_t sysid[6] = {0xaa, 0x00, 0x04, 0x00, 0x31, 0x05};
    struct peer_state *ps = peer_find_or_add(&w.cfg, &w.pdt, w.peers, mac);
    CHECK(ps != NULL, "peer slot");
    if (ps == NULL) {
        return;
    }
    ps_learn_sys_addr(&w.cfg, ps, sysid);
    (void)scs_pb_open(&w.cfg, ps->pb); /* vms-398: CONFIG_SYS selection needs an OPEN VC */

    CHECK(send_joiner_connect_request(7, 1, &w.cfg, ps, NULL, our_hw_mac, our_logical) == 1,
          "first send");
    CHECK(send_joiner_connect_request(7, 1, &w.cfg, ps, NULL, our_hw_mac, our_logical) == 1,
          "retransmit");
    CHECK(send_joiner_connect_request(7, 1, &w.cfg, ps, NULL, our_hw_mac, our_logical) == 1,
          "retransmit 2");

    CHECK(conn_transitions == 3, "%lu transitions recorded, expected 3", conn_transitions);
    CHECK(conn_illegal_events == 0, "a retransmit was scored illegal (%lu)",
          conn_illegal_events);
    CHECK(scs_conn_state_of(ps->cdt_joiner) == SCS_CONN_CONNECT_SENT,
          "a retransmit moved the state to %s",
          scs_conn_state_name(scs_conn_state_of(ps->cdt_joiner)));
    CHECK(scs_cdl_in_use_count(&scsd_cdl) == 1,
          "a retransmit allocated a second CDT (%u in use)",
          scs_cdl_in_use_count(&scsd_cdl));
}

/*
 * OVMX's three Con.IDs are node-global, so a SECOND peer cannot have its own
 * CDT at the same Con.ID. That must be visible, not silently wrong: conn_bind
 * refuses rather than allocating a Con.ID that differs from the one on the
 * wire. This is the limitation scs_cdt.h records, asserted rather than asserted
 * in prose.
 */
static void test_second_peer_connection_is_refused_not_faked(void)
{
    struct world w;
    world_init(&w);
    scs_cdl_init(&scsd_cdl);
    scsd_cdl_ready = 1;

    static const uint8_t mac_a[6] = {0x08, 0x00, 0x2b, 0x01, 0x01, 0x01};
    static const uint8_t mac_b[6] = {0x08, 0x00, 0x2b, 0x02, 0x02, 0x02};
    static const uint8_t sysid[6] = {0xaa, 0x00, 0x04, 0x00, 0x31, 0x05};

    struct peer_state *a = peer_find_or_add(&w.cfg, &w.pdt, w.peers, mac_a);
    struct peer_state *b = peer_find_or_add(&w.cfg, &w.pdt, w.peers, mac_b);
    CHECK(a != NULL && b != NULL && a != b, "two distinct peer slots");
    if (a == NULL || b == NULL) {
        return;
    }
    ps_learn_sys_addr(&w.cfg, a, sysid);
    ps_learn_sys_addr(&w.cfg, b, sysid);
    (void)scs_pb_open(&w.cfg, a->pb);
    (void)scs_pb_open(&w.cfg, b->pb);

    CHECK(send_joiner_connect_request(7, 1, &w.cfg, a, a->pb, our_hw_mac, our_logical) == 1,
          "peer A send");
    CHECK(send_joiner_connect_request(7, 1, &w.cfg, b, b->pb, our_hw_mac, our_logical) == 1,
          "peer B send");

    CHECK(a->cdt_joiner != NULL, "peer A got no CDT");
    CHECK(b->cdt_joiner == NULL,
          "peer B was given a CDT at a node-global Con.ID already claimed by peer A"
          " -- the CDL would then be describing the wrong connection");
    CHECK(scs_cdl_in_use_count(&scsd_cdl) == 1, "expected exactly one CDT in the CDL");
    /* Peer B still SENDS -- the machine is a recorder, never a gate. */
    CHECK(scsd_test_frames >= 2, "peer B's frame was suppressed by the state machine");
}

/* ==========================================================================
 * vms-fb1 / vms-dd5 -- THE RECEIVE DISPATCH, DRIVEN WITH REAL CAPTURED FRAMES.
 *
 * WHY THIS BLOCK EXISTS. The vms-dd5 adversary pass measured that three of the
 * four new conn_step() call sites, and the exit summary's
 * scs_conn_report_stuck() call, lived inside main()'s receive loop -- which
 * SCSD_UNIT_TEST renames away. They were compiled and never executed, so
 * mutating them did not red anything. src/vmsscs/scsd.c now hoists that loop
 * body into scsd_handle_frame() and the report into scsd_exit_summary(); these
 * tests call BOTH, with frames taken byte-exact off the reference-lab wire.
 *
 * PROVENANCE OF EVERY FRAME BELOW (rule 8: observation + public docs only):
 * all three were read out of
 *   /data/training/vax/cluster/captures/formation-ci1-joinwindow.pcap
 * -- the golden VAX2-joins-VAX1 formation -- with a pcap reader written for
 * this test, and are transcribed here wire-byte for wire-byte, Ethernet header
 * included. The pcap frame number is given for each. Frame #48 is the same
 * frame tests/vmsscs/test_scs_connect.c transcribes as `real_request`, which is
 * an independent cross-check that the reader read it right.
 * ========================================================================== */

/* pcap frame #30: VAX1 -> VAX2, SCS$DIRECTORY CONNECT-REQUEST. opcode 0x5b,
 * 110-byte SCA class, [46:48] message type 0 = CONNECT_REQ, destination Con.ID
 * 0 (VAX2's handle not yet known), source Con.ID 0x63050008. */
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

/* pcap frame #32: VAX2 -> VAX1, the answer to frame #30. opcode 0x5b, 66-byte
 * SCA class, [46:48] message type 1 = CONNECT_RSP -- Figure 2-14's bare
 * acknowledgement. Destination Con.ID 0x63050008 (VAX1's handle, echoed),
 * source Con.ID 0 (the 66-byte class carries none: 31/31 on the wire, see
 * test_scs_dir.c test_source_conid_p235). */
static const uint8_t cap_connect_rsp[80] = {
    0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9,
    0x60, 0x07, 0x40, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x5b, 0x13, 0x01, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x12, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x16, 0x00, 0x04, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x05, 0x63, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x53, 0x43, 0x53, 0x24
};

/* pcap frame #48: VAX1 -> VAX2, the VMS$VAXcluster CONNECT-REQUEST. opcode
 * 0x4b, 110-byte SCA class, [46:48] message type 0 = CONNECT_REQ, destination
 * Con.ID 0, source Con.ID 0x62C50009. This is the frame OVMX answers on the
 * MEMBER-opened connection. */
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

/* The MAC each capture is addressed to; the daemon only acts on frames unicast
 * to its own HW MAC, so the test wears the identity of the node that received
 * the frame rather than editing the frame. */
static const uint8_t vax2_hw_mac[6] = {0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9};
static const uint8_t vax1_hw_mac[6] = {0xaa, 0x00, 0x04, 0x00, 0x01, 0x04};
static const uint8_t vax1_logical[6] = {0xaa, 0x00, 0x04, 0x00, 0x01, 0x04};

/* ==========================================================================
 * THE MEMBER'S ANSWER TO *OVMX'S OWN* JOINER CONNECT-REQUEST, taken off the
 * wire of a run in which OVMX ITSELF was the joiner. These two frames are
 * addressed to Con.ID 0x4F580002 -- literally OVMX_JOINER_CONID -- so unlike
 * the VAX-to-VAX frames above they need NO edit of any kind: OVMX's own handle
 * is already in the bytes.
 *
 * PROVENANCE (rule 8: observation only): read with the same pcap reader out of
 *   /data/training/vax/cluster/captures/ovmx-760-MEMBER-achieved-20260730.pcap
 * -- the capture of the run in which OVMX reached full MEMBER. Frames #65 and
 * #67, transcribed wire-byte for wire-byte, Ethernet header included, ZERO
 * bytes edited. Both are VAX2 (08:00:2b:78:56:b9, SCS System Address
 * aa:00:04:00:9b:04) -> OVMX (b6:16:8a:dc:3a:53, aa:00:04:00:02:04).
 * ========================================================================== */

/* pcap frame #65: opcode 0x4b, 66-byte SCA class, [46:48] message type 1 =
 * CONNECT_RSP. Destination Con.ID 0x4F580002 = OVMX_JOINER_CONID (our handle,
 * echoed); source Con.ID 0 -- the 66-byte class carries none. */
static const uint8_t cap_ovmx_joiner_connect_rsp[80] = {
    0xb6, 0x16, 0x8a, 0xdc, 0x3a, 0x53, 0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9,
    0x60, 0x07, 0x40, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x9b, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x4b, 0x13, 0x0b, 0x00, 0x0b, 0x00,
    0x01, 0x00, 0x12, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00,
    0x0b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x16, 0x00, 0x04, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x58, 0x4f, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x56, 0x4d, 0x53, 0x24
};

/* pcap frame #67: opcode 0x4b, 110-byte SCA class, [46:48] message type 2 =
 * ACCEPT_REQ -- the frame that BINDS the pair. Destination Con.ID 0x4F580002 =
 * OVMX_JOINER_CONID, source Con.ID 0x63020011 = the member's own freshly
 * supplied handle, NON-ZERO. This is the specimen the production
 * `lconid != 0` guard requires, and it is real. */
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

/* The member's Con.ID in that dialogue, and the two identities the frames use. */
#define OVMX760_MEMBER_CONID 0x63020011u
static const uint8_t ovmx760_hw_mac[6] = {0xb6, 0x16, 0x8a, 0xdc, 0x3a, 0x53};
static const uint8_t ovmx760_logical[6] = {0xaa, 0x00, 0x04, 0x00, 0x02, 0x04};
static const uint8_t ovmx760_member_mac[6] = {0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9};
static const uint8_t ovmx760_member_sysid[6] = {0xaa, 0x00, 0x04, 0x00, 0x9b, 0x04};

/*
 * A complete receive-dispatch context, wired exactly the way main() wires one.
 * Nothing here re-implements the daemon: scsd_handle_frame() and
 * scsd_exit_summary() are the production functions, and this only supplies the
 * state main() owns.
 */
static void rxlog_reset(void); /* defined with the log-capture helpers below */

struct rxworld {
    struct world w;
    struct scs_hello_params hello_params;
    struct scsd_vc_ctx vc_ctx;
    struct scsd_rx rx;
    uint8_t hw_mac[6];
    uint8_t logical[6];
    uint8_t nonce[4];
};

static void rxworld_init(struct rxworld *r, const uint8_t hw_mac[6],
                         const uint8_t logical[6])
{
    static const uint8_t lab_nonce[4] = SCS_HELLO_LAB_NONCE_BYTES;
    memset(r, 0, sizeof(*r));
    world_init(&r->w);
    scs_cdl_init(&scsd_cdl);
    scsd_cdl_ready = 1;
    conn_transitions = 0;
    conn_illegal_events = 0;
    conn_unemitted_actions = 0;
    memcpy(r->hw_mac, hw_mac, 6);
    memcpy(r->logical, logical, 6);
    memcpy(r->nonce, lab_nonce, 4);

    r->vc_ctx.sock = 7;
    r->vc_ctx.ifindex = 1;
    r->vc_ctx.hw_mac = r->hw_mac;
    r->vc_ctx.src_logical = r->logical;
    r->vc_ctx.scssystemid = 1329;
    r->vc_ctx.node_name = "OVMX";
    r->vc_ctx.cfg = &r->w.cfg;

    r->rx.sock = 7;
    r->rx.ifindex = 1;
    r->rx.our_hw_mac = r->hw_mac;
    r->rx.our_src_logical = r->logical;
    r->rx.lab_nonce = r->nonce;
    r->rx.hello_params = &r->hello_params;
    r->rx.cfg = &r->w.cfg;
    r->rx.pdt = &r->w.pdt;
    r->rx.peers = r->w.peers;
    r->rx.vc_ctx = &r->vc_ctx;
    r->rx.ifname = "test0";
    r->rx.respond = 1;
    r->rx.do_connect = 1;
    r->rx.emit_hello = 0;

    /* vms-17f: the p. 2-20/2-21 open-transition tallies and the departure
     * counters are file-scope in scsd.c (see the comment there); a test that
     * reads them has to start from a known zero. */
    pb_open_results[0] = pb_open_results[1] = pb_open_results[2] = 0;
    pb_open_errors = 0;
    pb_open_masquerades = 0; /* vms-22e */
    peer_departures = 0;
    depart_connections_lost = 0;
    depart_refusals = 0;
    rxlog_reset();

    scsd_test_frames = 0;
    scsd_test_last_len = 0;
}

/*
 * The daemon logs every frame, so its output has to be taken off the terminal to
 * keep the test output readable. vms-17f KEEPS it instead of discarding it: the
 * SCSD-I-VCOPEN line names which p. 2-21 transition ran, and asserting that text
 * is what stops a mutant from swapping two clauses of scsd_open_result_text()
 * without reddening anything. `rxlog` accumulates until rxlog_reset().
 */
static char rxlog[262144];
static size_t rxlog_len = 0;

static void rxlog_reset(void)
{
    rxlog[0] = '\0';
    rxlog_len = 0;
}

static int rxlog_has(const char *needle)
{
    return strstr(rxlog, needle) != NULL;
}

/* How many times `needle` appears in the captured log. */
static unsigned rxlog_count(const char *needle)
{
    unsigned n = 0;
    for (const char *p = strstr(rxlog, needle); p != NULL; p = strstr(p + 1, needle)) {
        n++;
    }
    return n;
}

static int cap_saved_out = -1;
static int cap_saved_err = -1;
static FILE *cap_file = NULL;

static void log_capture_begin(void)
{
    fflush(stdout);
    fflush(stderr);
    cap_saved_out = dup(STDOUT_FILENO);
    cap_saved_err = dup(STDERR_FILENO);
    cap_file = tmpfile();
    if (cap_file != NULL) {
        dup2(fileno(cap_file), STDOUT_FILENO);
        dup2(fileno(cap_file), STDERR_FILENO);
    }
}

static void log_capture_end(void)
{
    fflush(stdout);
    fflush(stderr);
    if (cap_saved_out >= 0) {
        dup2(cap_saved_out, STDOUT_FILENO);
        close(cap_saved_out);
        cap_saved_out = -1;
    }
    if (cap_saved_err >= 0) {
        dup2(cap_saved_err, STDERR_FILENO);
        close(cap_saved_err);
        cap_saved_err = -1;
    }
    if (cap_file != NULL) {
        fflush(cap_file);
        rewind(cap_file);
        size_t room = sizeof(rxlog) - 1 - rxlog_len;
        size_t got = fread(rxlog + rxlog_len, 1, room, cap_file);
        rxlog_len += got;
        rxlog[rxlog_len] = '\0';
        fclose(cap_file);
        cap_file = NULL;
    }
}

/* Run the production dispatch with its logging captured into rxlog. */
static void rx_feed(struct rxworld *r, const uint8_t *frame, size_t len)
{
    log_capture_begin();
    scsd_handle_frame(&r->rx, frame, (ssize_t)len);
    log_capture_end();
}

/* Run the production departure sweep with its logging captured into rxlog. */
static unsigned rx_sweep(struct rxworld *r, uint64_t now_ms)
{
    log_capture_begin();
    unsigned departed = scsd_peer_departure_sweep(&r->rx, now_ms);
    log_capture_end();
    return departed;
}

/*
 * (1) THE SCS$DIRECTORY PAIR. Feeding the daemon the real captured
 * SCS$DIRECTORY CONNECT-REQUEST must bind a CDT at OVMX's directory Con.ID and
 * walk it CLOSED --RCV_CONNECT_REQ--> CONNECT REC --SVC_ACCEPT--> ACCEPT SENT,
 * which is Figure 2-14's NODE_2 column. Both conn_step() calls in that branch
 * are asserted by STATE and by CONID, not by log text.
 */
static void test_captured_directory_connect_drives_the_machine(void)
{
    struct rxworld r;
    rxworld_init(&r, vax2_hw_mac, our_logical);

    rx_feed(&r, cap_dir_connect_req, sizeof(cap_dir_connect_req));

    struct peer_state *ps = &r.w.peers[0];
    CHECK(ps->pb != NULL, "the captured directory frame created no peer");
    CHECK(ps->dir_connected == 1, "the daemon did not bind SCS$DIRECTORY");
    CHECK(r.rx.dir_conn_resp_sent == 1,
          "the daemon sent %ld directory CONNECT-RESPONSEs, expected 1",
          r.rx.dir_conn_resp_sent);

    CHECK(ps->cdt_dir != NULL,
          "no CDT was bound for the SCS$DIRECTORY connection -- the conn_bind in"
          " the receive loop did not run");
    if (ps->cdt_dir == NULL) {
        return;
    }
    CHECK(ps->cdt_dir->local_conid == SCS_DIR_OVMX_CONID,
          "the directory CDT claims Con.ID 0x%08X, expected 0x%08X",
          (unsigned)ps->cdt_dir->local_conid, (unsigned)SCS_DIR_OVMX_CONID);
    CHECK(ps->cdt_dir->remote_conid == 0x63050008u,
          "the directory CDT recorded remote Con.ID 0x%08X, but the captured frame"
          " supplied 0x63050008", (unsigned)ps->cdt_dir->remote_conid);
    CHECK(scs_conn_state_of(ps->cdt_dir) == SCS_CONN_ACCEPT_SENT,
          "after CONNECT_REQ + our accept the directory connection is %s,"
          " expected ACCEPT SENT",
          scs_conn_state_name(scs_conn_state_of(ps->cdt_dir)));
    CHECK(scs_cdl_lookup(&scsd_cdl, SCS_DIR_OVMX_CONID) == ps->cdt_dir,
          "the directory CDT is not reachable by its CONID through the CDL");
    CHECK(conn_transitions == 2,
          "%lu transitions recorded for the directory pair, expected 2",
          conn_transitions);
    CHECK(conn_illegal_events == 0, "the captured directory frame scored %lu illegal events",
          conn_illegal_events);
}

/*
 * (2) THE MEMBER-SIDE VMS$VAXcluster PAIR. The real captured 0x4b
 * CONNECT-REQUEST (destination Con.ID 0) must bind the member CDT at
 * OVMX_LOCAL_CONID and reach ACCEPT SENT, and the FIRST arrival must report one
 * unemitted action: the machine requires a CONNECT_RSP there and OVMX builds
 * none. That count is the honesty claim scsd.c makes in prose; here it is a
 * number.
 */
static void test_captured_member_connect_drives_the_machine(void)
{
    struct rxworld r;
    rxworld_init(&r, vax2_hw_mac, our_logical);

    rx_feed(&r, cap_vaxcluster_connect_req, sizeof(cap_vaxcluster_connect_req));

    struct peer_state *ps = &r.w.peers[0];
    CHECK(ps->pb != NULL, "the captured VMS$VAXcluster frame created no peer");
    CHECK(ps->connected == 1, "the daemon did not answer the member's CONNECT-REQUEST");
    CHECK(r.rx.connect_resp_sent == 1,
          "the daemon sent %ld CONNECT-RESPONSEs, expected 1", r.rx.connect_resp_sent);
    CHECK(ps->cdt_member != NULL,
          "no CDT was bound for the member-opened VMS$VAXcluster connection");
    if (ps->cdt_member == NULL) {
        return;
    }
    CHECK(ps->cdt_member->local_conid == OVMX_LOCAL_CONID,
          "the member CDT claims Con.ID 0x%08X, expected 0x%08X",
          (unsigned)ps->cdt_member->local_conid, (unsigned)OVMX_LOCAL_CONID);
    CHECK(ps->cdt_member->remote_conid == 0x62C50009u,
          "the member CDT recorded remote Con.ID 0x%08X, but the captured frame"
          " supplied 0x62C50009", (unsigned)ps->cdt_member->remote_conid);
    CHECK(scs_conn_state_of(ps->cdt_member) == SCS_CONN_ACCEPT_SENT,
          "the member connection is %s, expected ACCEPT SENT",
          scs_conn_state_name(scs_conn_state_of(ps->cdt_member)));
    CHECK(conn_transitions == 2, "%lu transitions on the member pair, expected 2",
          conn_transitions);
    CHECK(conn_unemitted_actions == 1,
          "%lu actions reported required-but-not-emitted, expected exactly 1"
          " (the CONNECT_RSP OVMX has no builder for)",
          conn_unemitted_actions);

    /* A RETRANSMITTED request is re-answered (vms-c6d) and must stay in ACCEPT
     * SENT through the labeled OVMX row -- and must NOT add a second unemitted
     * action, because the ACCEPT_REQ the row requires IS the frame just sent. */
    unsigned long unemitted_after_first = conn_unemitted_actions;
    rx_feed(&r, cap_vaxcluster_connect_req, sizeof(cap_vaxcluster_connect_req));
    CHECK(scs_conn_state_of(ps->cdt_member) == SCS_CONN_ACCEPT_SENT,
          "a retransmitted CONNECT-REQUEST moved the member connection to %s",
          scs_conn_state_name(scs_conn_state_of(ps->cdt_member)));
    CHECK(conn_illegal_events == 0, "the retransmit scored %lu illegal events",
          conn_illegal_events);
    CHECK(conn_unemitted_actions == unemitted_after_first,
          "the retransmit reported another unemitted action (%lu -> %lu)",
          unemitted_after_first, conn_unemitted_actions);
}

/*
 * (3) THE [46:48] CONNECTION-CONTROL CLASSIFIER. This is the branch that had no
 * other call site at all: before vms-dd5 the daemon did not react to a peer's
 * CONNECT_RSP, so a connection the peer had parked was invisible.
 *
 * FRAME PROVENANCE, exactly: cap_connect_rsp is pcap frame #32 byte for byte
 * EXCEPT the four bytes at [64:68], the destination Con.ID, which are retargeted
 * from VAX1's handle 0x63050008 to OVMX_JOINER_CONID. That edit is unavoidable
 * and it is the only one: OVMX's three Con.IDs are node-global constants, so a
 * frame addressed to OVMX cannot carry a VAX's handle. Everything the classifier
 * reads apart from that field -- the opcode at [30], the length, the [46:48]
 * message type -- is the captured wire.
 */
static void test_captured_connect_rsp_drives_the_classifier(void)
{
    struct rxworld r;
    /* OVMX stands in for the node the CONNECT_RSP was addressed to. */
    rxworld_init(&r, vax1_hw_mac, vax1_logical);

    /* Get a joiner connection into CONNECT SENT the production way. */
    static const uint8_t peer_mac[6] = {0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9};
    static const uint8_t peer_sysid[6] = {0xaa, 0x00, 0x04, 0x00, 0x02, 0x04};
    struct peer_state *ps = peer_find_or_add(&r.w.cfg, &r.w.pdt, r.w.peers, peer_mac);
    CHECK(ps != NULL, "peer slot");
    if (ps == NULL) {
        return;
    }
    ps_learn_sys_addr(&r.w.cfg, ps, peer_sysid);
    (void)scs_pb_open(&r.w.cfg, ps->pb);
    CHECK(send_joiner_connect_request(7, 1, &r.w.cfg, ps, NULL, r.hw_mac, r.logical) == 1,
          "the joiner CONNECT-REQUEST was not sent");
    CHECK(ps->cdt_joiner != NULL && scs_conn_state_of(ps->cdt_joiner) == SCS_CONN_CONNECT_SENT,
          "the joiner connection is not in CONNECT SENT before the CONNECT_RSP arrives");
    unsigned long transitions_before = conn_transitions;
    unsigned frames_before = scsd_test_frames;

    uint8_t frame[sizeof(cap_connect_rsp)];
    memcpy(frame, cap_connect_rsp, sizeof(frame));
    /* The ONLY edit: destination Con.ID -> OVMX's joiner handle (see above). */
    frame[64] = (uint8_t)(OVMX_JOINER_CONID & 0xff);
    frame[65] = (uint8_t)((OVMX_JOINER_CONID >> 8) & 0xff);
    frame[66] = (uint8_t)((OVMX_JOINER_CONID >> 16) & 0xff);
    frame[67] = (uint8_t)((OVMX_JOINER_CONID >> 24) & 0xff);
    /* The captured message type must still be 1 after the edit -- if this ever
     * reds, the frame was transcribed wrong and the test below proves nothing. */
    CHECK((frame[60] | (frame[61] << 8)) == 1,
          "the captured frame's [46:48] is %u, expected message type 1 (CONNECT_RSP)",
          (unsigned)(frame[60] | (frame[61] << 8)));

    rx_feed(&r, frame, sizeof(frame));

    CHECK(scs_conn_state_of(ps->cdt_joiner) == SCS_CONN_CONNECT_ACK,
          "after the peer's CONNECT_RSP the joiner connection is %s, expected"
          " CONNECT ACK (p. 2-23)",
          scs_conn_state_name(scs_conn_state_of(ps->cdt_joiner)));
    CHECK(conn_transitions == transitions_before + 1,
          "the classifier recorded %lu transitions, expected exactly 1",
          conn_transitions - transitions_before);
    CHECK(conn_illegal_events == 0, "the captured CONNECT_RSP scored %lu illegal events",
          conn_illegal_events);
    /* RECEIVE-SIDE ONLY: the classifier emits nothing of its own. The one frame
     * the dispatch does send here is the vms-691 SCS-layer 0x48 credit-return,
     * which every sequenced message gets 1-for-1 and which predates this item --
     * asserted by its opcode so a connection-control emission sneaking in would
     * red rather than hide behind a frame count. */
    CHECK(scsd_test_frames == frames_before + 1,
          "the dispatch transmitted %u frame(s), expected exactly the credit-return",
          scsd_test_frames - frames_before);
    CHECK(scsd_test_last_len > 30 && scsd_test_last_frame[30] == SCS_MSGTYPE_CREDIT,
          "the frame the dispatch sent has opcode 0x%02x, expected the 0x48"
          " credit-return -- the classifier must put nothing on the wire",
          scsd_test_last_len > 30 ? scsd_test_last_frame[30] : 0);
    /* This case STOPS at CONNECT ACK on purpose. pcap #32 is the 66-byte class
     * and carries source Con.ID 0, so production's `lconid != 0` guard rightly
     * refuses to bind on it. The Figure 2-14 completion to OPEN is driven by
     * the real ACCEPT_REQ in test_captured_ovmx_accept_req_opens_the_joiner()
     * below -- by the DAEMON, not by this test calling conn_step() itself. */
}

/*
 * (3b) THE ACCEPT_REQ THAT BINDS, DRIVEN BY PRODUCTION. Feeding the classifier
 * a CONNECT_RSP proves the classifier; it does NOT prove the JOINBOUND branch
 * that actually binds the joiner connection -- that branch needs a frame whose
 * SOURCE Con.ID is non-zero, which the 66-byte class never carries. So this
 * case uses the two frames the member sent to OVMX ITSELF in the run that
 * reached full MEMBER: #65 (CONNECT_RSP) then #67 (ACCEPT_REQ, source Con.ID
 * 0x63020011). Both are addressed to OVMX_JOINER_CONID on the wire, so NOTHING
 * IS EDITED. Every state change asserted below is performed by scsd.c.
 */
static void test_captured_ovmx_accept_req_opens_the_joiner(void)
{
    struct rxworld r;
    /* OVMX wears the identity it actually had in that capture. */
    rxworld_init(&r, ovmx760_hw_mac, ovmx760_logical);

    struct peer_state *ps =
        peer_find_or_add(&r.w.cfg, &r.w.pdt, r.w.peers, ovmx760_member_mac);
    CHECK(ps != NULL, "peer slot");
    if (ps == NULL) {
        return;
    }
    ps_learn_sys_addr(&r.w.cfg, ps, ovmx760_member_sysid);
    (void)scs_pb_open(&r.w.cfg, ps->pb);
    CHECK(send_joiner_connect_request(7, 1, &r.w.cfg, ps, NULL, r.hw_mac, r.logical) == 1,
          "the joiner CONNECT-REQUEST was not sent");
    CHECK(ps->cdt_joiner != NULL && scs_conn_state_of(ps->cdt_joiner) == SCS_CONN_CONNECT_SENT,
          "the joiner connection is not in CONNECT SENT before the member answers");

    /* The member's bare acknowledgement -- classifier only, no bind (its
     * source Con.ID is 0, and OVMX must not bind a null handle). */
    rx_feed(&r, cap_ovmx_joiner_connect_rsp, sizeof(cap_ovmx_joiner_connect_rsp));
    CHECK(scs_conn_state_of(ps->cdt_joiner) == SCS_CONN_CONNECT_ACK,
          "after the member's real CONNECT_RSP the joiner connection is %s,"
          " expected CONNECT ACK (p. 2-23)",
          scs_conn_state_name(scs_conn_state_of(ps->cdt_joiner)));
    CHECK(ps->joiner_connected == 0,
          "the 66-byte CONNECT_RSP (source Con.ID 0) bound the joiner connection;"
          " only the ACCEPT_REQ may do that");

    /* The ACCEPT_REQ. THIS is the frame that satisfies `lconid != 0`, so the
     * daemon itself runs scs_cdt_set_remote_conid() + conn_step(RCV_ACCEPT_REQ). */
    unsigned long transitions_before = conn_transitions;
    rx_feed(&r, cap_ovmx_joiner_accept_req, sizeof(cap_ovmx_joiner_accept_req));

    CHECK(scs_conn_state_of(ps->cdt_joiner) == SCS_CONN_OPEN,
          "the member's real ACCEPT_REQ left the joiner connection %s, expected"
          " OPEN (Figure 2-14)",
          scs_conn_state_name(scs_conn_state_of(ps->cdt_joiner)));
    CHECK(conn_transitions == transitions_before + 1,
          "the ACCEPT_REQ recorded %lu transitions, expected exactly 1",
          conn_transitions - transitions_before);
    CHECK(conn_illegal_events == 0,
          "the real join dialogue scored %lu illegal events", conn_illegal_events);
    /* The CDT must carry the member's handle off the wire, not a test constant. */
    CHECK(ps->cdt_joiner->remote_conid == OVMX760_MEMBER_CONID,
          "the joiner CDT's remote Con.ID is 0x%08X, expected the member's"
          " 0x%08X read out of the frame",
          ps->cdt_joiner->remote_conid,
          (unsigned)OVMX760_MEMBER_CONID);
    CHECK(ps->joiner_connected == 1 && ps->joiner_remote_conid == OVMX760_MEMBER_CONID,
          "the daemon did not record the joiner bind (connected=%d remote=0x%08X)",
          ps->joiner_connected, ps->joiner_remote_conid);
    /* And the bind is what releases the add-member burst on the joiner VC. */
    CHECK(r.rx.cm_config_frames > 0,
          "the joiner bind sent no add-member config frames");
}

/*
 * (3c) THE NEGATIVE CONTROL FOR THE DELETED DUPLICATE. scsd.c's branch (c)
 * used to carry a second copy of the joiner bind, which measurement showed
 * could only ever have run for a Con.ID-pair-class frame whose SOURCE Con.ID
 * was 0 -- a shape absent from all 41 lab captures, and wrong anyway (0 means
 * "handle not yet assigned"). The duplicate is deleted; this pins the
 * behaviour that replaces it, so re-adding it reds.
 *
 * SYNTHESIZED, and labeled as such: this frame is the REAL captured ACCEPT_REQ
 * with its source Con.ID field forced to 0. It is not a wire shape -- it is the
 * only input that could have reached the deleted branch, which is exactly why
 * it is the control.
 */
static void test_null_source_conid_binds_nothing(void)
{
    struct rxworld r;
    rxworld_init(&r, ovmx760_hw_mac, ovmx760_logical);

    struct peer_state *ps =
        peer_find_or_add(&r.w.cfg, &r.w.pdt, r.w.peers, ovmx760_member_mac);
    CHECK(ps != NULL, "peer slot");
    if (ps == NULL) {
        return;
    }
    ps_learn_sys_addr(&r.w.cfg, ps, ovmx760_member_sysid);
    (void)scs_pb_open(&r.w.cfg, ps->pb);
    CHECK(send_joiner_connect_request(7, 1, &r.w.cfg, ps, NULL, r.hw_mac, r.logical) == 1,
          "the joiner CONNECT-REQUEST was not sent");

    uint8_t frame[sizeof(cap_ovmx_joiner_accept_req)];
    memcpy(frame, cap_ovmx_joiner_accept_req, sizeof(frame));
    /* The ONLY edit: source Con.ID -> 0. Self-checked so a mistranscription of
     * the base frame cannot make this control vacuous. */
    uint32_t base_src_conid = (uint32_t)frame[68] | ((uint32_t)frame[69] << 8) |
                              ((uint32_t)frame[70] << 16) | ((uint32_t)frame[71] << 24);
    CHECK(base_src_conid == OVMX760_MEMBER_CONID,
          "the base frame's source Con.ID is 0x%08X, expected the member's 0x%08X"
          " -- the frame was transcribed wrong and this control proves nothing",
          base_src_conid, (unsigned)OVMX760_MEMBER_CONID);
    frame[68] = frame[69] = frame[70] = frame[71] = 0;

    rx_feed(&r, frame, sizeof(frame));

    CHECK(ps->joiner_connected == 0,
          "OVMX bound its joiner connection off a frame carrying source"
          " Con.ID 0 -- a null remote handle is not a handle");
    CHECK(ps->joiner_remote_conid == 0,
          "OVMX recorded remote Con.ID 0x%08X from a null-source frame",
          ps->joiner_remote_conid);
    CHECK(ps->cdt_joiner->remote_conid == 0,
          "the joiner CDT was bound to remote Con.ID 0x%08X off a null-source frame",
          ps->cdt_joiner->remote_conid);
    CHECK(scs_conn_state_of(ps->cdt_joiner) == SCS_CONN_CONNECT_SENT,
          "a null-source frame moved the joiner connection to %s; it must stay"
          " in CONNECT SENT",
          scs_conn_state_name(scs_conn_state_of(ps->cdt_joiner)));
    CHECK(r.rx.cm_config_frames == 0,
          "a null-source frame released the add-member burst");
}

/*
 * (4) THE EXIT SUMMARY. "A state that is entered and never left is detectable"
 * is only true if the detector RUNS. scsd_exit_summary() is the production
 * report; this drives it over a CDL holding a connection parked off OPEN and
 * asserts both halves: the transition accounting, and the named stuck
 * connection.
 *
 * This one DOES assert on the report's text, and that is not a workaround: the
 * text IS the product of a reporting function. The states it reports are
 * asserted structurally in (1)-(3) above.
 */
static void test_exit_summary_reports_the_parked_connection(void)
{
    struct rxworld r;
    rxworld_init(&r, vax2_hw_mac, our_logical);

    /* Same production path as (1): the directory connection ends in ACCEPT
     * SENT, which is exactly a connection that never reached OPEN. */
    rx_feed(&r, cap_dir_connect_req, sizeof(cap_dir_connect_req));
    struct peer_state *ps = &r.w.peers[0];
    CHECK(ps->cdt_dir != NULL && scs_conn_state_of(ps->cdt_dir) != SCS_CONN_OPEN,
          "the fixture did not park a connection off OPEN -- the summary below"
          " would have nothing to report");

    char buf[4096];
    buf[0] = '\0';
    FILE *cap = tmpfile();
    CHECK(cap != NULL, "tmpfile");
    if (cap == NULL) {
        return;
    }
    scsd_exit_summary(&r.rx, cap);
    fflush(cap);
    rewind(cap);
    size_t got = fread(buf, 1, sizeof(buf) - 1, cap);
    buf[got] = '\0';
    fclose(cap);

    char want_counters[128];
    snprintf(want_counters, sizeof(want_counters),
             "CONN-FSM: transitions=%lu illegal-events=%lu"
             " actions-required-but-not-emitted=%lu",
             conn_transitions, conn_illegal_events, conn_unemitted_actions);
    CHECK(strstr(buf, want_counters) != NULL,
          "the exit summary did not report the connection accounting ('%s')",
          want_counters);

    char want_stuck[160];
    snprintf(want_stuck, sizeof(want_stuck), "SCSD-W-CONNSTUCK, conid=0x%08X",
             (unsigned)SCS_DIR_OVMX_CONID);
    CHECK(strstr(buf, want_stuck) != NULL,
          "the exit summary did not NAME the connection parked off OPEN -- the"
          " stuck scan is not reached from production code ('%s' absent)",
          want_stuck);
    CHECK(strstr(buf, "1 of 1 in-use connection(s) parked off OPEN") != NULL,
          "the exit summary did not report the stuck COUNT");
    CHECK(strstr(buf, scs_conn_state_name(SCS_CONN_ACCEPT_SENT)) != NULL,
          "the exit summary did not report the state the connection is parked in");
}

/* ==========================================================================
 * vms-17f -- THE p. 2-21 REFRESH, REACHED BY THE DAEMON.
 *
 * PROVENANCE OF THE THREE FRAMES BELOW (rule 8: observation only). All three
 * are VAX1 -> VAX2 formation frames read out of
 *   /data/training/vax/cluster/captures/formation-ci1-joinwindow.pcap
 * -- the golden VAX2-joins-VAX1 formation -- with the same pcap reader as the
 * capture block above, transcribed wire-byte for wire-byte, Ethernet header
 * included, ZERO bytes edited. pcap frame numbers #24, #27, #28: the complete
 * peer half of one virtual-circuit formation dialogue (spec sec 4g phase 2,
 * rounds 0/1/2). Each is 0x41 at abs 30 and carries the config round at
 * abs [58:60].
 *
 * The test wears VAX2's HW MAC, exactly as the vms-fb1 cases do, because the
 * daemon only acts on frames unicast to its own MAC.
 * ========================================================================== */

/* pcap frame #24: VAX1 -> VAX2, round-0 START. 106-byte SCA class. */
static const uint8_t cap_vax1_start_round0[120] = {
    0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,
    0x60, 0x07, 0x68, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x41, 0x13, 0x00, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3e, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x40, 0x02, 0xd8, 0x00,
    0x56, 0x4d, 0x53, 0x20, 0x56, 0x37, 0x2e, 0x33, 0x66, 0x15, 0x66, 0x7a,
    0x93, 0x00, 0xbc, 0x00, 0x56, 0x41, 0x58, 0x20, 0x06, 0x00, 0x00, 0x0a,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x77, 0x00, 0x56, 0x41, 0x58, 0x31,
    0x20, 0x20, 0x20, 0x20, 0x80, 0x98, 0xb1, 0x55, 0x96, 0x00, 0xbc, 0x00
};

/* pcap frame #27: VAX1 -> VAX2, round-1 STACK. Identical to #24 except the
 * config round at [58:60]. */
static const uint8_t cap_vax1_stack_round1[120] = {
    0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,
    0x60, 0x07, 0x68, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x41, 0x13, 0x00, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3e, 0x00, 0x01, 0x00,
    0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x40, 0x02, 0xd8, 0x00,
    0x56, 0x4d, 0x53, 0x20, 0x56, 0x37, 0x2e, 0x33, 0x66, 0x15, 0x66, 0x7a,
    0x93, 0x00, 0xbc, 0x00, 0x56, 0x41, 0x58, 0x20, 0x06, 0x00, 0x00, 0x0a,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x77, 0x00, 0x56, 0x41, 0x58, 0x31,
    0x20, 0x20, 0x20, 0x20, 0x80, 0x98, 0xb1, 0x55, 0x96, 0x00, 0xbc, 0x00
};

/* pcap frame #28: VAX1 -> VAX2, the round-2 46-byte-class ack that completes
 * the dialogue. This is the frame the daemon answers with its own round-2. */
static const uint8_t cap_vax1_ack_round2[60] = {
    0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,
    0x60, 0x07, 0x2c, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x41, 0x13, 0x00, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00
};

/* Feed the peer's whole formation dialogue through the production dispatch. */
static void rx_feed_formation(struct rxworld *r)
{
    rx_feed(r, cap_vax1_start_round0, sizeof(cap_vax1_start_round0));
    rx_feed(r, cap_vax1_stack_round1, sizeof(cap_vax1_stack_round1));
    rx_feed(r, cap_vax1_ack_round2, sizeof(cap_vax1_ack_round2));
}

/* The peer slot the daemon built for VAX1, or NULL if it released it. */
static struct peer_state *rx_peer_of(struct rxworld *r, const uint8_t mac[6])
{
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        if (r->w.peers[i].pb != NULL && mac_eq(r->w.peers[i].pb->remote_port_addr, mac)) {
            return &r->w.peers[i];
        }
    }
    return NULL;
}

/*
 * THE HEADLINE CASE. A node forms a circuit, goes silent past the listen
 * timeout, and comes back with the SAME identity -- all of it through
 * scsd_handle_frame() and scsd_peer_departure_sweep(), the two production
 * functions the daemon's main loop calls, with captured frames.
 *
 * Nothing here performs a transition by hand. The test supplies frames and a
 * clock; every scs_pb_open(), scs_pb_depart() and slot release is scsd.c's.
 */
static void test_rejoin_reaches_the_p221_refresh(void)
{
    struct rxworld r;
    unsetenv("OVMX_NO_PEER_DEPART");
    unsetenv("OVMX_PEER_LISTEN_TIMEOUT_MS");
    rxworld_init(&r, vax2_hw_mac, our_logical);

    /* ---- 1. THE FIRST JOIN. ---- */
    rx_feed_formation(&r);

    struct peer_state *ps = rx_peer_of(&r, vax1_hw_mac);
    CHECK(ps != NULL, "the daemon built no peer slot for the captured formation");
    if (ps == NULL) {
        return;
    }
    CHECK(ps->start_acked == 1, "the daemon did not complete the START dialogue");
    CHECK(pb_open_results[SCS_OPEN_NEW_SB] == 1,
          "the first join produced %lu NEW_SB transitions, expected 1",
          pb_open_results[SCS_OPEN_NEW_SB]);
    CHECK(pb_open_results[SCS_OPEN_EXISTING_REFRESHED] == 0,
          "a FIRST join took the rejoin REFRESH");
    CHECK(scs_config_sb_count(&r.w.cfg) == 1, "the peer's System Block was not queued");
    unsigned frames_after_first_join = scsd_test_frames;
    CHECK(frames_after_first_join == 3,
          "the first join emitted %u frames, expected the 3 of a formation dialogue",
          frames_after_first_join);
    struct scs_sb *sb_before = ps->pb->sb;
    uint8_t sysid_before[6];
    memcpy(sysid_before, sb_before->system_id, 6);
    CHECK(memcmp(sysid_before, vax1_logical, 6) == 0,
          "the SB does not carry the SCS System Address the captured frames advertise");

    /* ---- 2. THE NEGATIVE CONTROL: a peer that has just spoken is not gone. ----
     * Same sweep, same code, one argument different. Without this the assertion
     * below would pass for a sweep that departs everyone unconditionally. */
    uint64_t heard_at = ps->last_rx_ms;
    uint64_t timeout = scs_depart_listen_timeout_ms();
    CHECK(heard_at != 0, "the daemon never stamped the peer's last-heard time");
    CHECK(rx_sweep(&r, heard_at) == 0, "a peer heard from this instant was declared departed");
    CHECK(rx_sweep(&r, heard_at + timeout - 1) == 0,
          "a peer silent for one ms less than the listen timeout was declared departed");
    CHECK(peer_departures == 0, "the control sweeps departed %lu peers", peer_departures);
    CHECK(rx_peer_of(&r, vax1_hw_mac) == ps, "a control sweep released the peer slot");

    /* ---- 3. THE DEPARTURE. ---- */
    CHECK(rx_sweep(&r, heard_at + timeout) == 1,
          "a peer silent for exactly the listen timeout was NOT declared departed");
    CHECK(peer_departures == 1, "the sweep recorded %lu departures, expected 1",
          peer_departures);
    CHECK(rx_peer_of(&r, vax1_hw_mac) == NULL, "the departed peer kept its slot");
    /* And the slot is released WHOLE. A free slot is recognised by pb == NULL
     * alone, so anything else left in it is a departed node's state sitting
     * where a reader that does not check would take it for a live peer's. */
    {
        struct peer_state empty;
        memset(&empty, 0, sizeof(empty));
        int residue = 0;
        for (int i = 0; i < OVMX_MAX_PEERS; i++) {
            if (memcmp(&r.w.peers[i], &empty, sizeof(empty)) != 0) {
                residue = 1;
            }
        }
        CHECK(residue == 0,
              "the released peer slot still carries state from the departed node");
    }
    CHECK(scs_pdt_formative_count(&r.w.pdt) == 0, "the closed Path Block was left on the PDT");
    CHECK(scs_cdl_in_use_count(&scsd_cdl) == 0,
          "a connection survived the departure of the circuit that carried it");
    /* p. 2-17: the System Block is the node's memory and it must SURVIVE -- that
     * is the whole premise of the p. 2-21 Note. */
    CHECK(scs_config_sb_count(&r.w.cfg) == 1,
          "the System Block was dropped with the circuit, so nothing is left for a"
          " rejoin to refresh");
    CHECK(rxlog_has("SCSD-I-PEERGONE"), "the departure was not logged");

    /* ---- 4. THE REJOIN, SAME IDENTITY, SAME CAPTURED FRAMES. ---- */
    rxlog_reset();
    rx_feed_formation(&r);

    struct peer_state *back = rx_peer_of(&r, vax1_hw_mac);
    CHECK(back != NULL, "the returning node got no peer slot");
    CHECK(pb_open_results[SCS_OPEN_EXISTING_REFRESHED] == 1,
          "the rejoin produced %lu REFRESH transitions, expected 1 --"
          " SCS_OPEN_EXISTING_REFRESHED is still unreachable from the daemon",
          pb_open_results[SCS_OPEN_EXISTING_REFRESHED]);
    CHECK(pb_open_results[SCS_OPEN_NEW_SB] == 1,
          "the rejoin learned the node again instead of refreshing its System Block");
    CHECK(pb_open_results[SCS_OPEN_EXISTING_SB] == 0,
          "the rejoin took the already-open early return -- the pre-vms-17f behaviour");
    CHECK(scs_config_sb_count(&r.w.cfg) == 1, "the rejoin duplicated the System Block");
    CHECK(back != NULL && back->pb != NULL && back->pb->sb == sb_before,
          "the rejoin attached to a different System Block -- the old one was not"
          " refreshed, it was replaced");
    /* The log branch is LIVE, and it says the right thing. A mutant that swaps
     * two clauses of scsd_open_result_text() dies here. */
    CHECK(rxlog_has("old system block REFRESHED (rejoin, p. 2-21 Note)"),
          "the daemon did not log the REFRESH transition");
    CHECK(rxlog_count("SCSD-I-VCOPEN") == 1,
          "the rejoin logged %u open transitions, expected exactly 1",
          rxlog_count("SCSD-I-VCOPEN"));
    CHECK(!rxlog_has("node learned for the first time"),
          "the rejoin logged the FIRST-CONTACT clause");
    CHECK(!rxlog_has("UNEXPECTED"),
          "a log line still claims a transition is unreachable");

    /* THE WIRE-VISIBLE CONSEQUENCE, which is the whole reason this item ships a
     * kill switch: the returning node is answered with a second, complete
     * formation dialogue. Before vms-17f OVMX emitted nothing at all, because it
     * believed the circuit was still open. */
    CHECK(scsd_test_frames == frames_after_first_join + 3,
          "the rejoin emitted %u frames in total, expected %u (a second formation"
          " dialogue on top of the first)",
          scsd_test_frames, frames_after_first_join + 3);
}

/*
 * THE KILL SWITCH, RUN AT DAEMON LEVEL (guardrail 23), and it is the same
 * fixture as above so the ONLY difference is the environment variable.
 *
 * With OVMX_NO_PEER_DEPART=1 the daemon must behave exactly as it did before
 * vms-17f: no departure, no slot release, and -- the part that matters on the
 * wire -- NOT ONE FRAME emitted in answer to the returning node's formation
 * dialogue, because the circuit is still believed open.
 */
static void test_departure_kill_switch_restores_the_pinned_slot(void)
{
    struct rxworld r;
    setenv("OVMX_NO_PEER_DEPART", "1", 1);
    unsetenv("OVMX_PEER_LISTEN_TIMEOUT_MS");
    rxworld_init(&r, vax2_hw_mac, our_logical);

    rx_feed_formation(&r);
    struct peer_state *ps = rx_peer_of(&r, vax1_hw_mac);
    CHECK(ps != NULL, "the daemon built no peer slot with the switch set");
    if (ps == NULL) {
        unsetenv("OVMX_NO_PEER_DEPART");
        return;
    }
    CHECK(pb_open_results[SCS_OPEN_NEW_SB] == 1, "the first join did not run under the switch");
    unsigned frames_after_first_join = scsd_test_frames;

    /* However long the peer is silent, nothing happens. */
    uint64_t way_past = ps->last_rx_ms + 100u * scs_depart_listen_timeout_ms();
    CHECK(rx_sweep(&r, way_past) == 0, "OVMX_NO_PEER_DEPART=1 still departed a peer");
    CHECK(peer_departures == 0, "the switch let %lu departures through", peer_departures);
    CHECK(rx_peer_of(&r, vax1_hw_mac) == ps, "the switch still released the peer slot");
    CHECK(ps->pb->in_use == 1, "the switch still closed the Path Block");
    CHECK(!rxlog_has("SCSD-I-PEERGONE"), "the switch still logged a departure");

    /* The returning node re-enters the SAME slot on the SAME open circuit, and
     * OVMX answers its formation dialogue with silence. */
    rx_feed_formation(&r);
    CHECK(rx_peer_of(&r, vax1_hw_mac) == ps, "the switch did not pin the peer to its slot");
    CHECK(pb_open_results[SCS_OPEN_EXISTING_REFRESHED] == 0,
          "the REFRESH ran with OVMX_NO_PEER_DEPART=1 set -- the switch does not gate it");
    CHECK(scsd_test_frames == frames_after_first_join,
          "the gated daemon emitted %u frames to the returning node, expected 0 more"
          " than the %u of the first join",
          scsd_test_frames - frames_after_first_join, frames_after_first_join);

    /* BRACKET: unset it, feed the same frames to a fresh fixture, and the
     * behaviour comes back. Without this the case above is equally consistent
     * with "the rejoin never works". */
    unsetenv("OVMX_NO_PEER_DEPART");
    struct rxworld r2;
    rxworld_init(&r2, vax2_hw_mac, our_logical);
    rx_feed_formation(&r2);
    struct peer_state *ps2 = rx_peer_of(&r2, vax1_hw_mac);
    CHECK(ps2 != NULL, "bracket: no peer slot");
    if (ps2 == NULL) {
        return;
    }
    unsigned frames_before = scsd_test_frames;
    CHECK(rx_sweep(&r2, ps2->last_rx_ms + scs_depart_listen_timeout_ms()) == 1,
          "bracket: the peer did not depart with the switch unset");
    rx_feed_formation(&r2);
    CHECK(pb_open_results[SCS_OPEN_EXISTING_REFRESHED] == 1,
          "bracket: the REFRESH did not run with the switch unset");
    CHECK(scsd_test_frames == frames_before + 3,
          "bracket: the returning node was not answered with a formation dialogue");
}

/*
 * The listen timeout is not a hidden constant: OVMX_PEER_LISTEN_TIMEOUT_MS moves
 * it, which is how a lab run forces a departure inside a short window, and the
 * daemon's own accessor is what the sweep uses.
 */
static void test_listen_timeout_override_moves_the_departure(void)
{
    struct rxworld r;
    unsetenv("OVMX_NO_PEER_DEPART");
    setenv("OVMX_PEER_LISTEN_TIMEOUT_MS", "2000", 1);
    rxworld_init(&r, vax2_hw_mac, our_logical);
    rx_feed_formation(&r);
    struct peer_state *ps = rx_peer_of(&r, vax1_hw_mac);
    CHECK(ps != NULL, "no peer slot");
    if (ps == NULL) {
        unsetenv("OVMX_PEER_LISTEN_TIMEOUT_MS");
        return;
    }
    uint64_t heard_at = ps->last_rx_ms;
    CHECK(scs_depart_listen_timeout_ms() == 2000, "the override did not reach the daemon");
    CHECK(rx_sweep(&r, heard_at + 1999) == 0, "departed 1ms before the overridden timeout");
    CHECK(rx_sweep(&r, heard_at + 2000) == 1, "did not depart at the overridden timeout");
    unsetenv("OVMX_PEER_LISTEN_TIMEOUT_MS");
}

/*
 * A peer that is only BEACONING is not departed. The daemon acts on frames
 * unicast to its own MAC, but liveness is a broader question than "is it talking
 * to me": stamping only the frames the responder handles would age out a member
 * that is up and multicasting. peer_touch() runs before that gate, and this is
 * the case that holds it there.
 */
static void test_multicast_beacon_keeps_a_peer_alive(void)
{
    struct rxworld r;
    unsetenv("OVMX_NO_PEER_DEPART");
    unsetenv("OVMX_PEER_LISTEN_TIMEOUT_MS");
    rxworld_init(&r, vax2_hw_mac, our_logical);
    rx_feed_formation(&r);
    struct peer_state *ps = rx_peer_of(&r, vax1_hw_mac);
    CHECK(ps != NULL, "no peer slot");
    if (ps == NULL) {
        return;
    }
    /* Wind the peer's last-heard time back to the very start of the clock, so
     * that "it has been silent long enough to depart" is true by construction
     * and the ONLY thing that can rescue it is the beacon below. */
    ps->last_rx_ms = 1;
    uint64_t timeout = scs_depart_listen_timeout_ms();

    /* The SAME captured round-0 START, re-addressed to the HELLO multicast group
     * -- i.e. a frame from this peer that the responder will NOT act on, because
     * it is not unicast to us. Only the destination MAC changes. */
    uint8_t beacon[sizeof(cap_vax1_start_round0)];
    memcpy(beacon, cap_vax1_start_round0, sizeof(beacon));
    const uint8_t hello_group[6] = {0xab, 0x00, 0x04, 0x01, 0x01, 0x01};
    memcpy(beacon, hello_group, 6);

    unsigned frames_before = scsd_test_frames;
    rx_feed(&r, beacon, sizeof(beacon));
    CHECK(scsd_test_frames == frames_before,
          "the daemon answered a frame that was not addressed to it");
    CHECK(ps->last_rx_ms > 1,
          "a multicast frame from a live peer did not refresh its last-heard time --"
          " liveness is being taken from the unicast responder path only");

    /* The peer is therefore still alive as of the beacon, not as of the last
     * frame it addressed to us: a sweep well past the old timestamp's deadline
     * leaves it alone. */
    CHECK(rx_sweep(&r, 1 + timeout) == 0, "a beaconing peer was declared departed");
    CHECK(rx_peer_of(&r, vax1_hw_mac) == ps, "a beaconing peer lost its slot");
}

/*
 * =====================================================================
 * vms-22e: WHAT THE DAEMON DOES WITH AN ABANDONED OPEN.
 *
 * scs_config.c owns the p. 2-21 footnote RULE and tests/vmsscs/test_scs_config.c
 * proves it. This case is about the OTHER half of the item's done condition:
 * that an abandonment "is logged with WHICH test failed", and that the daemon
 * does not go on to announce a circuit it just refused. Neither of those lives
 * in scs_config.c -- both are the SCSD-W-VCMASQ branch of scsd_vc_on_open().
 *
 * Left untested, three separate mutations of that branch survive the whole of
 * this file: deleting it outright, logging a CONSTANT test name instead of the
 * failing one, and dropping its early `return` so STARTDONE/VCOPEN print on a
 * refused circuit. Each of those is a real failure mode -- the last one is an
 * operator being told a virtual circuit is OPEN when it is not.
 *
 * WHY THIS CANNOT BE DRIVEN BY CAPTURED FRAMES, stated so the seeding below is
 * not read as a shortcut: struct scs_start_view carries no SCS Node Name and no
 * incarnation, so every System Block the daemon builds from the wire has an
 * empty name and a zero incarnation, every footnote comparison is INDETERMINATE
 * and scs_config_masquerade_check() returns PASS. That is asserted directly in
 * step 0 below rather than asserted in prose. The named formative SB is
 * therefore attached through the production entry point
 * (scs_pb_attach_formative_sb) to the production peer slot's Path Block, and
 * the transition itself is performed by production scsd_vc_on_open().
 * =====================================================================
 */
static void test_masquerade_open_is_logged_and_suppresses_vcopen(void)
{
    struct world w;
    world_init(&w);
    uint8_t victim_mac[6], impostor_mac[6], sysid[6];
    mac_of(0x91, victim_mac);
    mac_of(0x92, impostor_mac);
    sysid_of(1025, sysid);
    struct scsd_vc_ctx ctx = vc_test_ctx(&w);

    pb_open_results[0] = pb_open_results[1] = pb_open_results[2] = 0;
    pb_open_errors = 0;
    pb_open_masquerades = 0;
    rxlog_reset();

    /* ---- 0. THE REACHABILITY CLAIM, MEASURED. A peer built exactly the way
     * the receive path builds one -- system address and nothing else -- opens
     * cleanly against a queued node with the SAME System ID. No footnote test
     * can fire on daemon-shaped input, which is why this case has to seed. ---- */
    {
        struct world d;
        world_init(&d);
        uint8_t mac_a[6], mac_b[6];
        mac_of(0x9a, mac_a);
        mac_of(0x9b, mac_b);
        struct peer_state *p1 = peer_find_or_add(&d.cfg, &d.pdt, d.peers, mac_a);
        CHECK(p1 != NULL, "no peer slot for the daemon-shape control");
        if (p1 == NULL) {
            return;
        }
        ps_learn_sys_addr(&d.cfg, p1, sysid);
        CHECK(p1->pb->sb != NULL && p1->pb->sb->node_name[0] == '\0' &&
                  p1->pb->sb->incarnation == 0,
              "the receive path now supplies a node name or an incarnation -- this"
              " case's premise (all three tests indeterminate in production) is"
              " stale and the reachability comment on scsd_vc_on_open() must be"
              " re-derived");
        CHECK(scs_pb_open(&d.cfg, p1->pb) == SCS_OPEN_NEW_SB, "control first join failed");
        struct peer_state *p2 = peer_find_or_add(&d.cfg, &d.pdt, d.peers, mac_b);
        CHECK(p2 != NULL, "no second peer slot for the daemon-shape control");
        if (p2 == NULL) {
            return;
        }
        ps_learn_sys_addr(&d.cfg, p2, sysid);
        CHECK(scs_pb_open(&d.cfg, p2->pb) != SCS_OPEN_ABANDONED_MASQUERADE,
              "a daemon-shaped System Block was convicted -- the branch under test"
              " IS reachable from the wire and the comment saying it is not is now"
              " wrong");
    }

    /* ---- 1. A KNOWN NODE, named, with its circuit open. ---- */
    struct peer_state *victim = peer_find_or_add(&w.cfg, &w.pdt, w.peers, victim_mac);
    CHECK(victim != NULL, "no peer slot for the victim node");
    if (victim == NULL) {
        return;
    }
    struct scs_sb_info known;
    memset(&known, 0, sizeof(known));
    memcpy(known.system_id, sysid, 6);
    known.node_name = "VAX1";
    known.incarnation = 0x1000ull;
    known.cpu_type = 7;
    CHECK(scs_pb_attach_formative_sb(&w.cfg, victim->pb, &known) != NULL,
          "could not attach the victim's formative System Block");
    scs_vc_init(&victim->vc);
    log_capture_begin();
    scsd_vc_on_open(&ctx, victim);
    log_capture_end();
    CHECK(pb_open_results[SCS_OPEN_NEW_SB] == 1,
          "the victim's join produced %lu NEW_SB transitions, expected 1",
          pb_open_results[SCS_OPEN_NEW_SB]);
    CHECK(rxlog_has("SCSD-I-VCOPEN"),
          "an ADMITTED circuit did not announce VCOPEN -- the suppression"
          " assertion below would then pass for a daemon that never logs it");
    CHECK(rxlog_has("SCSD-I-STARTDONE"), "an admitted circuit did not log STARTDONE");
    CHECK(pb_open_masquerades == 0, "an admitted circuit counted as a masquerade");

    /* ---- 2. THE IMPOSTOR: VAX1's System ID under another name. ---- */
    rxlog_reset();
    struct peer_state *impostor = peer_find_or_add(&w.cfg, &w.pdt, w.peers, impostor_mac);
    CHECK(impostor != NULL, "no peer slot for the impostor");
    if (impostor == NULL) {
        return;
    }
    struct scs_sb_info fake = known;
    fake.node_name = "EVIL";
    CHECK(scs_pb_attach_formative_sb(&w.cfg, impostor->pb, &fake) != NULL,
          "could not attach the impostor's formative System Block");
    scs_vc_init(&impostor->vc);
    log_capture_begin();
    scsd_vc_on_open(&ctx, impostor);
    log_capture_end();

    CHECK(pb_open_masquerades == 1,
          "the daemon recorded %lu abandoned opens, expected 1 -- the"
          " SCSD-W-VCMASQ branch did not run", pb_open_masquerades);
    CHECK(rxlog_has("SCSD-W-VCMASQ"),
          "an abandoned virtual-circuit formation was NOT logged; log was: '%s'",
          rxlog);
    /* WHICH test failed, by its own text -- not merely "a masquerade happened".
     * A branch that logged a constant would pass the line above. */
    CHECK(rxlog_has(scs_masquerade_result_name(SCS_MASQ_FAIL_NODE_NAME)),
          "the log does not name the failing test (%s); log was: '%s'",
          scs_masquerade_result_name(SCS_MASQ_FAIL_NODE_NAME), rxlog);
    CHECK(!rxlog_has(scs_masquerade_result_name(SCS_MASQ_PASS)),
          "the abandonment was logged as PASS; log was: '%s'", rxlog);
    /* THE SUPPRESSION: a refused circuit is not announced as open. */
    CHECK(!rxlog_has("SCSD-I-VCOPEN"),
          "the daemon announced VCOPEN on a circuit it had just ABANDONED;"
          " log was: '%s'", rxlog);
    CHECK(!rxlog_has("SCSD-I-STARTDONE"),
          "the daemon announced STARTDONE on an abandoned circuit; log was: '%s'",
          rxlog);
    CHECK(pb_open_results[SCS_OPEN_NEW_SB] == 1 &&
              pb_open_results[SCS_OPEN_EXISTING_SB] == 0 &&
              pb_open_results[SCS_OPEN_EXISTING_REFRESHED] == 0 &&
              pb_open_errors == 0,
          "an abandoned open was tallied as a completed transition"
          " (new=%lu existing=%lu refreshed=%lu errors=%lu)",
          pb_open_results[SCS_OPEN_NEW_SB], pb_open_results[SCS_OPEN_EXISTING_SB],
          pb_open_results[SCS_OPEN_EXISTING_REFRESHED], pb_open_errors);
    CHECK(scs_config_sb_count(&w.cfg) == 1,
          "the abandoned formative System Block was inserted into the"
          " configuration queue");

    /* ---- 3. A DIFFERENT FAILING TEST MUST PRODUCE DIFFERENT TEXT. This is
     * what makes step 2's assertion about naming the test load-bearing: a
     * branch that logs any fixed string cannot satisfy both. ---- */
    rxlog_reset();
    uint8_t third_mac[6];
    mac_of(0x93, third_mac);
    struct peer_state *third = peer_find_or_add(&w.cfg, &w.pdt, w.peers, third_mac);
    CHECK(third != NULL, "no peer slot for the incarnation impostor");
    if (third == NULL) {
        return;
    }
    struct scs_sb_info wrong_inc = known;
    wrong_inc.incarnation = 0x2000ull; /* same ID, same name, PB still queued */
    CHECK(scs_pb_attach_formative_sb(&w.cfg, third->pb, &wrong_inc) != NULL,
          "could not attach the incarnation impostor's System Block");
    scs_vc_init(&third->vc);
    log_capture_begin();
    scsd_vc_on_open(&ctx, third);
    log_capture_end();
    CHECK(pb_open_masquerades == 2, "the second abandonment was not counted (%lu)",
          pb_open_masquerades);
    CHECK(rxlog_has(scs_masquerade_result_name(SCS_MASQ_FAIL_INCARNATION)),
          "the log names the wrong failing test -- expected '%s'; log was: '%s'",
          scs_masquerade_result_name(SCS_MASQ_FAIL_INCARNATION), rxlog);
    CHECK(!rxlog_has(scs_masquerade_result_name(SCS_MASQ_FAIL_NODE_NAME)),
          "the log still names the PREVIOUS failing test -- the clause is a"
          " constant, not the result; log was: '%s'", rxlog);
    CHECK(!rxlog_has("SCSD-I-VCOPEN"), "VCOPEN printed on the second abandonment");
}

int main(void)
{
    /* Several assertions below assume the machine starts enabled. */
    (void)unsetenv("OVMX_NO_CONN_FSM");

    test_learned_system_address_is_the_peer_logical_field();
    test_port_address_and_system_address_stay_distinct();
    test_undiscovered_system_address_is_zero();
    test_peer_slot_identity_and_capacity();
    test_shared_sb_aliases_the_peer_logical();
    test_sb_exhaustion_is_visible_and_unreachable();
    test_released_peer_slot_gives_a_returning_node_a_new_circuit();
    /* vms-4071: the daemon's use of the VC formation state machine. */
    test_vc_actions_emit_the_right_config_rounds();
    test_vc_happy_path_frame_sequence();
    test_vc_early_ack_is_opt_in();
    test_vc_open_on_bare_ack_still_sends_round_2();
    test_vc_implied_ack_through_the_daemon();
    test_vc_reissue_and_abandon_through_the_daemon();
    test_connect_selects_the_open_vc_via_config_sys();
    /* vms-dd5: the daemon's use of the connection state machine + the CDL. */
    test_conn_fsm_does_not_change_the_wire();
    test_joiner_retransmit_is_not_an_illegal_event();
    test_second_peer_connection_is_refused_not_faked();
    /* vms-fb1: the SAME machine, driven through the daemon's receive dispatch
     * with frames taken byte-exact off the reference-lab wire. */
    test_captured_directory_connect_drives_the_machine();
    test_captured_member_connect_drives_the_machine();
    test_captured_connect_rsp_drives_the_classifier();
    test_captured_ovmx_accept_req_opens_the_joiner();
    test_null_source_conid_binds_nothing();
    test_exit_summary_reports_the_parked_connection();
    /* vms-17f: peer departure, and the p. 2-21 REFRESH the daemon can now
     * reach, driven by captured formation frames through the same dispatch. */
    test_rejoin_reaches_the_p221_refresh();
    test_departure_kill_switch_restores_the_pinned_slot();
    test_listen_timeout_override_moves_the_departure();
    test_multicast_beacon_keeps_a_peer_alive();
    /* vms-22e: the daemon's half of the p. 2-21 footnote rule -- the log line
     * that names the failing test, and the VCOPEN it must NOT print. */
    test_masquerade_open_is_logged_and_suppresses_vcopen();

    CHECK(peer_logical_offset > 0,
          "the peer-logical offset was never located -- the offset-dependent"
          " assertions above did not run");

    printf("test_scsd_wire: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
