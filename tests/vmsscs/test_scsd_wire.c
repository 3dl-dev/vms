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
 * SCOPE HONESTY: this exercises SCSD's frame-assembly path, not its receive loop
 * or socket setup. A lab join capture is the end-to-end proof and is a separate
 * activity; nothing here claims to be one.
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
    if (!send_joiner_connect_request(7 /* fd never touched under the seam */, 1, ps,
                                     our_hw_mac, our_logical)) {
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
    CHECK(send_joiner_connect_request(7, 1, ps, our_hw_mac, our_logical) == 1,
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
    CHECK(send_joiner_connect_request(7, 1, pa, our_hw_mac, our_logical) == 1,
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
    CHECK(send_joiner_connect_request(7, 1, ps, our_hw_mac, our_logical) == 1,
          "sender refused to build a frame");
    if (peer_logical_offset > 0 && scsd_test_frames == 1) {
        CHECK(is_zero6(scsd_test_last_frame + peer_logical_offset),
              "degraded peer did not emit a zero peer-logical");
        CHECK(memcmp(scsd_test_last_dst, mac, 6) == 0,
              "degraded peer lost its destination port address too");
    }
}

/*
 * The p. 2-21 REFRESH transition is INERT in the daemon. This asserts the reason
 * mechanically so the claim in scsd.c and scs_config.h cannot rot silently: a
 * rejoining node re-enters the same peer slot on the same already-open Path
 * Block, and the second open takes the early return instead of the REFRESH.
 * Making it fire is wire-visible and is filed as vms-17f.
 */
static void test_rejoin_is_inert_in_the_daemon(void)
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

    /* The node reboots and comes back on the same Ethernet port. SCSD's lookup is
     * by port address and never releases a slot, so this is the SAME slot and the
     * SAME Path Block -- there is no second circuit to run the transition on. */
    struct peer_state *again = peer_find_or_add(&w.cfg, &w.pdt, w.peers, mac);
    CHECK(again == ps, "rejoin allocated a new peer slot");
    CHECK(again != NULL && again->pb == first_pb, "rejoin allocated a new Path Block");
    CHECK(scs_pdt_formative_count(&w.pdt) == 0, "rejoin re-queued a formative PB");
    CHECK(scs_pb_open(&w.cfg, ps->pb) == SCS_OPEN_EXISTING_SB,
          "a rejoin on the same PB no longer takes the already-open early return");
    CHECK(scs_config_sb_count(&w.cfg) == 1, "rejoin duplicated the System Block");
    /* i.e. SCS_OPEN_EXISTING_REFRESHED is never produced: the refresh rule is
     * implemented and tested in scs_config.c, but OVMX does not run it. */
}

int main(void)
{
    test_learned_system_address_is_the_peer_logical_field();
    test_port_address_and_system_address_stay_distinct();
    test_undiscovered_system_address_is_zero();
    test_peer_slot_identity_and_capacity();
    test_shared_sb_aliases_the_peer_logical();
    test_sb_exhaustion_is_visible_and_unreachable();
    test_rejoin_is_inert_in_the_daemon();

    CHECK(peer_logical_offset > 0,
          "the peer-logical offset was never located -- the offset-dependent"
          " assertions above did not run");

    printf("test_scsd_wire: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
