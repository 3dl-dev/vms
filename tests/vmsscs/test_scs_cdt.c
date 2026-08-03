/*
 * test_scs_cdt.c - CDT / CDL / Path-Block connection-queue tests (vms-e1a).
 *
 * Asserts the documented model of Roy G. Davis, *VAXcluster Principles*,
 * Digital Press 1993, ch. 2:
 *   - a CDT carries the local and remote SYSAP names, the connection state,
 *     the SB and PB pointers, connect data, the VC-loss error handler and the
 *     message and datagram input routines                        (pp. 2-28, 2-29)
 *   - each CDT has a 32-bit local CONID; the remote CONID is learned during
 *     formation                                                        (p. 2-28)
 *   - the LOW 16 BITS of a CONID index the CDL to find the CDT       (p. 2-29)
 *   - ALL CDTs for connections on a circuit are queued to that circuit's Path
 *     Block, so VC loss can enumerate them and notify their SYSAPs   (p. 2-28)
 *   - CDL sizing SCSCONNCNT + 200, with the initial SCSCONNCNT CDTs
 *     pre-placed, released-but-not-deallocated reuse, and up to 200 overflow
 *     CDTs allocated as needed                                          (p. 2-30)
 *   - a packet is routed to the right SYSAP input routine by DESTINATION
 *     CONID, and its SOURCE CONID identifies the sender          (pp. 2-29, 2-35)
 *
 * Pure state: this test builds no frame and opens no socket.
 */
#include <stdio.h>
#include <string.h>

#include "scs_cdt.h"
#include "scs_connect.h" /* SCS_CONNECT_OVMX_CONID_BASE -- the shipped wire value */
#include "scs_dir.h"     /* SCS_DIR_OVMX_CONID          -- the shipped wire value */

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

/* The CONID tag must stay equal to the Con.ID base scsd.c has been putting on
 * the lab wire since vms-5fe, or the CDL would hand out CONIDs that differ from
 * the bytes OVMX ships. Compile-time, so drift cannot reach a test run. */
#if (SCS_CONNECT_OVMX_CONID_BASE >> 16) != SCS_CDT_CONID_TAG
#error "SCS_CDT_CONID_TAG drifted from the Con.ID base scsd.c puts on the wire"
#endif

/* --- SYSAP stubs: the message / datagram / VC-loss routines a real SYSAP
 * would supply to CONNECT and ACCEPT (pp. 2-28..2-29). They record what they
 * were handed so the test can prove WHICH routine of WHICH CDT ran. --- */
struct sysap_record {
    int msgs;
    int dgrams;
    int losses;
    uint32_t last_msg_conid;   /* local CONID of the CDT the message arrived on */
    uint32_t last_dgram_conid;
    uint32_t last_loss_conid;
    char last_payload[32];
    size_t last_len;
};

static void rec_msg(struct scs_cdt *cdt, const void *buf, size_t len, void *ctx)
{
    struct sysap_record *r = (struct sysap_record *)ctx;
    r->msgs++;
    r->last_msg_conid = cdt->local_conid;
    r->last_len = len;
    memset(r->last_payload, 0, sizeof(r->last_payload));
    if (buf != NULL && len < sizeof(r->last_payload)) {
        memcpy(r->last_payload, buf, len);
    }
}

static void rec_dgram(struct scs_cdt *cdt, const void *buf, size_t len, void *ctx)
{
    struct sysap_record *r = (struct sysap_record *)ctx;
    (void)buf;
    r->dgrams++;
    r->last_dgram_conid = cdt->local_conid;
    r->last_len = len;
}

static void rec_loss(struct scs_cdt *cdt, void *ctx)
{
    struct sysap_record *r = (struct sysap_record *)ctx;
    r->losses++;
    r->last_loss_conid = cdt->local_conid;
}

/* --- a minimal open circuit to hang connections off ----------------------- */

static const uint8_t mac_a[6] = {0x08, 0x00, 0x2b, 0x11, 0x22, 0x33};
static const uint8_t mac_b[6] = {0x08, 0x00, 0x2b, 0x44, 0x55, 0x66};
static const uint8_t sysid_a[6] = {0xaa, 0x00, 0x04, 0x00, 0x31, 0x05};
static const uint8_t sysid_b[6] = {0xaa, 0x00, 0x04, 0x00, 0x32, 0x05};

/* Build an OPEN virtual circuit to the node identified by `sysid`, exactly the
 * way scs_config.c's documented path does (discover -> learn -> open). */
static struct scs_pb *open_circuit(struct scs_config *cfg, struct scs_pdt *pdt,
                                   const uint8_t mac[6], const uint8_t sysid[6])
{
    struct scs_pb *pb = scs_pb_create(cfg, pdt, mac, SCS_PORT_TYPE_ETHERNET);
    if (pb == NULL) {
        return NULL;
    }
    if (scs_pb_learn_system_addr(cfg, pb, sysid) == NULL) {
        return NULL;
    }
    if (scs_pb_open(cfg, pb) == SCS_OPEN_ERROR) {
        return NULL;
    }
    return pb;
}

/* ========================================================================== */

/*
 * p. 2-30: "The number of entries in the CDL is equal to the value of the
 * SYSGEN parameter SCSCONNCNT plus 200"; the first SCSCONNCNT entries hold the
 * pre-allocated, marked-unused CDTs.
 */
static void test_cdl_init_sizing(void)
{
    static struct scs_cdl cdl;
    scs_cdl_init(&cdl);

    CHECK(SCS_CDL_ENTRIES == SCS_CDT_SCSCONNCNT + 200,
          "CDL is %d entries, expected SCSCONNCNT+200 = %d", (int)SCS_CDL_ENTRIES,
          (int)(SCS_CDT_SCSCONNCNT + 200));

    unsigned populated = 0, in_use = 0;
    for (unsigned i = 0; i < SCS_CDL_ENTRIES; i++) {
        if (cdl.entry[i] != NULL) {
            populated++;
            if (cdl.entry[i]->in_use) {
                in_use++;
            }
        }
    }
    CHECK(populated == SCS_CDT_SCSCONNCNT,
          "init populated %u CDL entries, expected the first SCSCONNCNT = %u", populated,
          (unsigned)SCS_CDT_SCSCONNCNT);
    CHECK(in_use == 0, "init left %u CDTs marked in use, expected all unused", in_use);
    CHECK(cdl.overflow_allocated == 0, "init allocated %u overflow CDTs, expected 0",
          cdl.overflow_allocated);
    CHECK(scs_cdl_in_use_count(&cdl) == 0, "fresh CDL reports connections in use");
}

/*
 * p. 2-28/2-29: the CDT carries every listed item, its local CONID identifies
 * it, and the CONID's low 16 bits index the CDL back to it.
 */
static void test_conid_indexes_cdl_to_cdt(void)
{
    static struct scs_config cfg;
    static struct scs_pdt pdt;
    static struct scs_cdl cdl;
    struct sysap_record rec;

    memset(&rec, 0, sizeof(rec));
    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 4096);
    scs_cdl_init(&cdl);

    struct scs_pb *pb = open_circuit(&cfg, &pdt, mac_a, sysid_a);
    CHECK(pb != NULL, "could not open a circuit to hang connections off");

    struct scs_cdt *cdt =
        scs_cdl_alloc(&cdl, "VMS$VAXcluster  ", "VMS$VAXcluster  ", pb);
    CHECK(cdt != NULL, "scs_cdl_alloc returned NULL on a fresh CDL");
    if (cdt == NULL) {
        return;
    }

    /* Every p. 2-28 item is present and filled. */
    CHECK(strcmp(cdt->local_sysap, "VMS$VAXcluster  ") == 0, "local SYSAP name is '%s'",
          cdt->local_sysap);
    CHECK(strcmp(cdt->remote_sysap, "VMS$VAXcluster  ") == 0, "remote SYSAP name is '%s'",
          cdt->remote_sysap);
    CHECK(cdt->pb == pb, "CDT does not point at the Path Block of its circuit");
    CHECK(cdt->sb == pb->sb && cdt->sb != NULL,
          "CDT does not point at the System Block for the node at the far end");
    CHECK(cdt->conn_state == 0, "a fresh CDT's connection state is not the zero state");
    CHECK(cdt->remote_conid == 0, "a fresh CDT already claims to know the remote CONID");

    /* The CONID is 32 bits and its low 16 index the CDL (p. 2-29). */
    CHECK(cdt->local_conid != 0, "CDT was given a zero CONID");
    unsigned slot = SCS_CDT_CONID_SLOT(cdt->local_conid);
    CHECK(slot < SCS_CDL_ENTRIES, "CONID slot %u is outside the CDL", slot);
    CHECK(cdl.entry[slot] == cdt, "CDL slot %u does not hold this CDT's address", slot);
    CHECK(scs_cdl_lookup(&cdl, cdt->local_conid) == cdt,
          "CONID 0x%08X did not resolve back to its CDT", cdt->local_conid);

    /* The remote CONID is learned during formation, not at allocation. */
    scs_cdt_set_remote_conid(cdt, 0x33580008u);
    CHECK(cdt->remote_conid == 0x33580008u, "remote CONID not recorded");

    /* Connect data is carried verbatim (content is vms-fdd's). */
    const uint8_t cd[4] = {0xde, 0xad, 0xbe, 0xef};
    CHECK(scs_cdt_set_connect_data(cdt, cd, sizeof(cd)) == 0, "connect data refused");
    CHECK(cdt->connect_data_len == 4 && memcmp(cdt->connect_data, cd, 4) == 0,
          "connect data not carried verbatim");
    CHECK(scs_cdt_set_connect_data(cdt, cd, SCS_CDT_CONNECT_DATA_MAX + 1) == -1,
          "oversized connect data accepted");

    /* The three SYSAP entry points are stored in the CDT (pp. 2-28..2-29). */
    scs_cdt_set_handlers(cdt, rec_msg, rec_dgram, rec_loss, &rec);
    CHECK(cdt->msg_input == rec_msg && cdt->dgram_input == rec_dgram &&
              cdt->vc_loss_handler == rec_loss && cdt->sysap_ctx == &rec,
          "CDT did not store the SYSAP entry points");

    /* An unknown CONID resolves to nothing. */
    CHECK(scs_cdl_lookup(&cdl, SCS_CDT_MAKE_CONID(SCS_CDL_ENTRIES - 1)) == NULL,
          "an unallocated CONID resolved to a CDT");
    CHECK(scs_cdl_lookup(&cdl, 0) == NULL, "CONID 0 resolved to a CDT");
    /* Right slot, wrong tag: the high 16 bits are load-bearing. */
    CHECK(scs_cdl_lookup(&cdl, (cdt->local_conid & 0xFFFFu) | 0xDEAD0000u) == NULL,
          "a CONID with the wrong tag resolved via its slot index");
}

/*
 * p. 2-28: "SCA specifies that all CDTs corresponding to connections supported
 * by a virtual circuit be queued to the Path Block corresponding to that
 * circuit." Two connections on one circuit; both reachable FROM the PB.
 */
static void test_two_cdts_on_one_pb(void)
{
    static struct scs_config cfg;
    static struct scs_pdt pdt;
    static struct scs_cdl cdl;

    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 4096);
    scs_cdl_init(&cdl);

    struct scs_pb *pb_a = open_circuit(&cfg, &pdt, mac_a, sysid_a);
    struct scs_pb *pb_b = open_circuit(&cfg, &pdt, mac_b, sysid_b);
    CHECK(pb_a != NULL && pb_b != NULL, "could not open two circuits");
    if (pb_a == NULL || pb_b == NULL) {
        return;
    }

    struct scs_cdt *vc = scs_cdl_alloc(&cdl, "VMS$VAXcluster  ", "VMS$VAXcluster  ", pb_a);
    struct scs_cdt *dir = scs_cdl_alloc(&cdl, "SCS$DIRECTORY   ", "SCS$DIRECTORY   ", pb_a);
    struct scs_cdt *other = scs_cdl_alloc(&cdl, "MSCP$DISK       ", "MSCP$DISK       ", pb_b);
    CHECK(vc != NULL && dir != NULL && other != NULL, "allocation of three CDTs failed");
    if (vc == NULL || dir == NULL || other == NULL) {
        return;
    }

    CHECK(vc->local_conid != dir->local_conid && dir->local_conid != other->local_conid &&
              vc->local_conid != other->local_conid,
          "CONIDs collided: 0x%08X 0x%08X 0x%08X", vc->local_conid, dir->local_conid,
          other->local_conid);

    /* Both of circuit A's connections are reachable by walking the PB queue. */
    CHECK(scs_pb_cdt_count(pb_a) == 2, "circuit A carries %u connections, expected 2",
          scs_pb_cdt_count(pb_a));
    CHECK(scs_pb_cdt_count(pb_b) == 1, "circuit B carries %u connections, expected 1",
          scs_pb_cdt_count(pb_b));

    int saw_vc = 0, saw_dir = 0, saw_foreign = 0;
    for (struct scs_cdt *c = scs_pb_first_cdt(pb_a); c != NULL; c = scs_cdt_next_on_pb(c)) {
        if (c == vc) {
            saw_vc = 1;
        } else if (c == dir) {
            saw_dir = 1;
        } else {
            saw_foreign = 1;
        }
        CHECK(c->pb == pb_a, "a CDT on circuit A's queue points at a different PB");
    }
    CHECK(saw_vc && saw_dir, "walking circuit A's PB queue did not reach both connections");
    CHECK(!saw_foreign, "circuit A's PB queue contained a connection from another circuit");

    /* Releasing the middle of the queue leaves the other reachable (p. 2-30:
     * released, not deallocated -- the CDL slot keeps the address). */
    unsigned dir_slot = SCS_CDT_CONID_SLOT(dir->local_conid);
    uint32_t dir_conid = dir->local_conid;
    scs_cdl_release(&cdl, dir);
    CHECK(scs_pb_cdt_count(pb_a) == 1, "release did not dequeue the CDT from its PB");
    CHECK(scs_pb_first_cdt(pb_a) == vc, "the surviving connection is no longer reachable");
    CHECK(cdl.entry[dir_slot] == dir, "released CDT was deallocated out of its CDL slot");
    CHECK(scs_cdl_lookup(&cdl, dir_conid) == NULL,
          "a released connection's CONID still resolves");
}

/*
 * pp. 2-29/2-35: a packet is delivered to the input routine named by the
 * DESTINATION CONID, and the SOURCE CONID says who sent it.
 */
static void test_packet_routed_by_destination_conid(void)
{
    static struct scs_config cfg;
    static struct scs_pdt pdt;
    static struct scs_cdl cdl;
    struct sysap_record rec_vc, rec_dir;

    memset(&rec_vc, 0, sizeof(rec_vc));
    memset(&rec_dir, 0, sizeof(rec_dir));
    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 4096);
    scs_cdl_init(&cdl);

    struct scs_pb *pb = open_circuit(&cfg, &pdt, mac_a, sysid_a);
    CHECK(pb != NULL, "could not open a circuit");
    if (pb == NULL) {
        return;
    }

    struct scs_cdt *vc = scs_cdl_alloc(&cdl, "VMS$VAXcluster  ", "VMS$VAXcluster  ", pb);
    struct scs_cdt *dir = scs_cdl_alloc(&cdl, "SCS$DIRECTORY   ", "SCS$DIRECTORY   ", pb);
    CHECK(vc != NULL && dir != NULL, "allocation failed");
    if (vc == NULL || dir == NULL) {
        return;
    }
    scs_cdt_set_handlers(vc, rec_msg, rec_dgram, rec_loss, &rec_vc);
    scs_cdt_set_handlers(dir, rec_msg, rec_dgram, rec_loss, &rec_dir);
    /* Peer CONIDs learned during formation (p. 2-28). */
    scs_cdt_set_remote_conid(vc, 0x62C50009u);
    scs_cdt_set_remote_conid(dir, 0x63050008u);

    /* Two connections, ONE circuit, same SYSAP routine address: only the
     * destination CONID can tell them apart. */
    CHECK(scs_cdl_deliver_message(&cdl, vc->local_conid, 0x62C50009u, "CLUSTER", 7) ==
              SCS_DELIVER_OK,
          "message to the VMS$VAXcluster connection was not delivered");
    CHECK(rec_vc.msgs == 1 && rec_dir.msgs == 0,
          "message went to the wrong SYSAP (vc=%d dir=%d)", rec_vc.msgs, rec_dir.msgs);
    CHECK(rec_vc.last_msg_conid == vc->local_conid,
          "the routine was handed the wrong CDT");
    CHECK(rec_vc.last_len == 7 && memcmp(rec_vc.last_payload, "CLUSTER", 7) == 0,
          "payload was not passed through");

    CHECK(scs_cdl_deliver_message(&cdl, dir->local_conid, 0x63050008u, "LOOKUP", 6) ==
              SCS_DELIVER_OK,
          "message to the SCS$DIRECTORY connection was not delivered");
    CHECK(rec_dir.msgs == 1 && rec_vc.msgs == 1,
          "second message went to the wrong SYSAP (vc=%d dir=%d)", rec_vc.msgs,
          rec_dir.msgs);

    /* A datagram fetches the OTHER routine from the same CDT (p. 2-29). */
    CHECK(scs_cdl_deliver_datagram(&cdl, vc->local_conid, 0x62C50009u, "DG", 2) ==
              SCS_DELIVER_OK,
          "datagram not delivered");
    CHECK(rec_vc.dgrams == 1 && rec_vc.msgs == 1,
          "datagram was routed to the message input routine");

    /* Source CONID: p. 2-35. A packet claiming to come from a different remote
     * CDT than this connection's is refused, not delivered. */
    CHECK(scs_cdl_deliver_message(&cdl, vc->local_conid, 0x63050008u, "X", 1) ==
              SCS_DELIVER_SRC_MISMATCH,
          "a packet with a foreign source CONID was delivered anyway");
    CHECK(rec_vc.msgs == 1, "the refused packet still reached the SYSAP");
    /* Source CONID 0 means "the frame class does not carry one" (the 66-byte
     * class on the real wire) and must not be treated as a mismatch. */
    CHECK(scs_cdl_deliver_message(&cdl, vc->local_conid, 0, "Y", 1) == SCS_DELIVER_OK,
          "a frame with no source CONID was refused");
    CHECK(rec_vc.msgs == 2, "the zero-source packet was not delivered");

    /* No CDT / no routine. */
    CHECK(scs_cdl_deliver_message(&cdl, SCS_CDT_MAKE_CONID(SCS_CDL_ENTRIES - 1), 0, "Z",
                                  1) == SCS_DELIVER_NO_CDT,
          "a message to an unknown destination CONID was not refused");
    struct scs_cdt *mute = scs_cdl_alloc(&cdl, "MSCP$TAPE       ", "MSCP$TAPE       ", pb);
    CHECK(mute != NULL, "third allocation failed");
    if (mute != NULL) {
        CHECK(scs_cdl_deliver_message(&cdl, mute->local_conid, 0, "Z", 1) ==
                  SCS_DELIVER_NO_ROUTINE,
              "a message to a CDT with no input routine was reported delivered");
    }
}

/*
 * p. 2-28: "If the circuit is broken for any reason, it is then a relatively
 * simple matter to scan this queue to determine which connections have also
 * been lost, and to notify the interested SYSAPs."
 */
static void test_vc_loss_notifies_every_connection_on_the_circuit(void)
{
    static struct scs_config cfg;
    static struct scs_pdt pdt;
    static struct scs_cdl cdl;
    struct sysap_record rec_a1, rec_a2, rec_b;

    memset(&rec_a1, 0, sizeof(rec_a1));
    memset(&rec_a2, 0, sizeof(rec_a2));
    memset(&rec_b, 0, sizeof(rec_b));
    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 4096);
    scs_cdl_init(&cdl);

    struct scs_pb *pb_a = open_circuit(&cfg, &pdt, mac_a, sysid_a);
    struct scs_pb *pb_b = open_circuit(&cfg, &pdt, mac_b, sysid_b);
    CHECK(pb_a != NULL && pb_b != NULL, "could not open two circuits");
    if (pb_a == NULL || pb_b == NULL) {
        return;
    }

    struct scs_cdt *a1 = scs_cdl_alloc(&cdl, "VMS$VAXcluster  ", "VMS$VAXcluster  ", pb_a);
    struct scs_cdt *a2 = scs_cdl_alloc(&cdl, "MSCP$DISK       ", "MSCP$DISK       ", pb_a);
    struct scs_cdt *b1 = scs_cdl_alloc(&cdl, "VMS$VAXcluster  ", "VMS$VAXcluster  ", pb_b);
    CHECK(a1 != NULL && a2 != NULL && b1 != NULL, "allocation failed");
    if (a1 == NULL || a2 == NULL || b1 == NULL) {
        return;
    }
    scs_cdt_set_handlers(a1, NULL, NULL, rec_loss, &rec_a1);
    scs_cdt_set_handlers(a2, NULL, NULL, rec_loss, &rec_a2);
    scs_cdt_set_handlers(b1, NULL, NULL, rec_loss, &rec_b);

    unsigned lost = scs_cdl_vc_loss(pb_a);
    CHECK(lost == 2, "VC loss on circuit A reported %u lost connections, expected 2", lost);
    CHECK(rec_a1.losses == 1 && rec_a2.losses == 1,
          "not every SYSAP on the broken circuit was notified (%d,%d)", rec_a1.losses,
          rec_a2.losses);
    CHECK(rec_a1.last_loss_conid == a1->local_conid &&
              rec_a2.last_loss_conid == a2->local_conid,
          "a VC-loss handler was handed the wrong CDT");
    CHECK(rec_b.losses == 0, "a connection on the OTHER circuit was notified of VC loss");

    /* A handler releasing its own connection mid-scan must not break the walk. */
    scs_cdl_release(&cdl, a2);
    lost = scs_cdl_vc_loss(pb_a);
    CHECK(lost == 1, "after releasing one connection the scan found %u, expected 1", lost);
    CHECK(scs_cdl_vc_loss(NULL) == 0, "VC loss on a NULL PB reported connections");
}

/*
 * p. 2-30: the initial SCSCONNCNT CDTs are used first and reused when released;
 * only when they are exhausted are the 200 overflow CDTs allocated as needed,
 * and the CDL is full at SCSCONNCNT + 200.
 */
static void test_two_tier_allocation_and_exhaustion(void)
{
    static struct scs_cdl cdl;
    scs_cdl_init(&cdl);

    /* Slot 0 is reserved (OVMX design choice), so the initial allocation yields
     * SCSCONNCNT-1 usable CDTs before the overflow tier is touched. */
    unsigned initial_usable = SCS_CDT_SCSCONNCNT - 1;
    for (unsigned i = 0; i < initial_usable; i++) {
        struct scs_cdt *c = scs_cdl_alloc(&cdl, "SYSAP", "SYSAP", NULL);
        CHECK(c != NULL, "initial allocation ran out early at %u", i);
        if (c == NULL) {
            return;
        }
        CHECK(SCS_CDT_CONID_SLOT(c->local_conid) < SCS_CDT_SCSCONNCNT,
              "CDT %u came from the overflow tier before the initial one was exhausted", i);
    }
    CHECK(cdl.overflow_allocated == 0,
          "overflow CDTs were allocated while the initial allocation still had room");

    /* Release one and re-allocate: it must come back from the initial tier
     * ("released (but not deallocated) so that they can then be reused"). */
    struct scs_cdt *reuse_target = cdl.entry[3];
    scs_cdl_release(&cdl, reuse_target);
    struct scs_cdt *reused = scs_cdl_alloc(&cdl, "SYSAP", "SYSAP", NULL);
    CHECK(reused == reuse_target, "a released CDT was not reused for the next connection");
    CHECK(cdl.overflow_allocated == 0, "reuse fell through to the overflow tier");

    /* Now exhaust the overflow tier too. */
    for (unsigned i = 0; i < SCS_CDT_CDL_OVERFLOW; i++) {
        struct scs_cdt *c = scs_cdl_alloc(&cdl, "SYSAP", "SYSAP", NULL);
        CHECK(c != NULL, "overflow allocation ran out early at %u", i);
        if (c == NULL) {
            return;
        }
        CHECK(SCS_CDT_CONID_SLOT(c->local_conid) >= SCS_CDT_SCSCONNCNT,
              "overflow CDT %u came from the initial tier", i);
    }
    CHECK(cdl.overflow_allocated == SCS_CDT_CDL_OVERFLOW,
          "allocated %u overflow CDTs, expected %u", cdl.overflow_allocated,
          (unsigned)SCS_CDT_CDL_OVERFLOW);
    CHECK(scs_cdl_alloc(&cdl, "SYSAP", "SYSAP", NULL) == NULL,
          "the CDL handed out more than SCSCONNCNT + 200 connections");
    CHECK(scs_cdl_in_use_count(&cdl) == initial_usable + SCS_CDT_CDL_OVERFLOW,
          "in-use count is %u, expected %u", scs_cdl_in_use_count(&cdl),
          initial_usable + SCS_CDT_CDL_OVERFLOW);
}

/*
 * The three Con.IDs OVMX has been putting on the lab wire since vms-5fe/vms-246
 * are legal CDL slots under the p. 2-29 rule, so adopting the CDL costs no wire
 * change. This is the concrete claim behind scs_cdt.h's WIRE VERDICT.
 */
static void test_shipped_wire_conids_are_valid_cdl_slots(void)
{
    static struct scs_cdl cdl;
    scs_cdl_init(&cdl);

    const uint32_t shipped[3] = {
        SCS_CONNECT_OVMX_CONID_BASE | 0x0001u, /* scsd.c OVMX_LOCAL_CONID  */
        SCS_CONNECT_OVMX_CONID_BASE | 0x0002u, /* scsd.c OVMX_JOINER_CONID */
        SCS_DIR_OVMX_CONID                     /* scs_dir.h SCS$DIRECTORY  */
    };
    const char *names[3] = {"VMS$VAXcluster  ", "VMS$VAXcluster  ", "SCS$DIRECTORY   "};

    for (unsigned i = 0; i < 3; i++) {
        struct scs_cdt *c = scs_cdl_alloc_conid(&cdl, shipped[i], names[i], names[i], NULL);
        CHECK(c != NULL, "shipped Con.ID 0x%08X could not claim its CDL slot", shipped[i]);
        if (c == NULL) {
            continue;
        }
        CHECK(c->local_conid == shipped[i],
              "claiming 0x%08X produced CONID 0x%08X -- the wire value would change",
              shipped[i], c->local_conid);
        CHECK(scs_cdl_lookup(&cdl, shipped[i]) == c,
              "shipped Con.ID 0x%08X does not resolve through the CDL", shipped[i]);
    }

    /* Claiming a live CONID twice, or one this node could not have issued, fails. */
    CHECK(scs_cdl_alloc_conid(&cdl, shipped[0], "X", "X", NULL) == NULL,
          "a Con.ID already in use was handed out a second time");
    CHECK(scs_cdl_alloc_conid(&cdl, 0xDEAD0001u, "X", "X", NULL) == NULL,
          "a Con.ID with a foreign tag was accepted as ours");
    CHECK(scs_cdl_alloc_conid(&cdl, SCS_CDT_MAKE_CONID(0), "X", "X", NULL) == NULL,
          "CDL slot 0 was handed out");
}

/* Re-binding a connection between circuits, and NULL safety. */
static void test_rebind_and_null_safety(void)
{
    static struct scs_config cfg;
    static struct scs_pdt pdt;
    static struct scs_cdl cdl;

    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 4096);
    scs_cdl_init(&cdl);

    struct scs_pb *pb_a = open_circuit(&cfg, &pdt, mac_a, sysid_a);
    struct scs_pb *pb_b = open_circuit(&cfg, &pdt, mac_b, sysid_b);
    CHECK(pb_a != NULL && pb_b != NULL, "could not open two circuits");
    if (pb_a == NULL || pb_b == NULL) {
        return;
    }

    struct scs_cdt *c = scs_cdl_alloc(&cdl, "SYSAP", "SYSAP", NULL);
    CHECK(c != NULL && c->pb == NULL && c->sb == NULL,
          "a CDT allocated without a circuit is already bound to one");
    if (c == NULL) {
        return;
    }
    scs_cdt_set_pb(c, pb_a);
    CHECK(c->pb == pb_a && c->sb == pb_a->sb && scs_pb_cdt_count(pb_a) == 1,
          "binding a connection to a circuit did not queue it to the PB");
    scs_cdt_set_pb(c, pb_b);
    CHECK(scs_pb_cdt_count(pb_a) == 0 && scs_pb_cdt_count(pb_b) == 1,
          "re-binding left the connection on both circuits");
    CHECK(c->sb == pb_b->sb, "re-binding did not re-sync the System Block pointer");
    scs_cdt_set_pb(c, NULL);
    CHECK(scs_pb_cdt_count(pb_b) == 0 && c->pb == NULL && c->sb == NULL,
          "unbinding left the connection queued");

    /* NULL safety: none of these may crash. */
    scs_cdl_init(NULL);
    scs_cdl_release(NULL, NULL);
    scs_cdl_release(&cdl, NULL);
    scs_cdt_set_remote_conid(NULL, 1);
    scs_cdt_set_handlers(NULL, NULL, NULL, NULL, NULL);
    scs_cdt_set_pb(NULL, pb_a);
    CHECK(scs_cdt_set_connect_data(NULL, NULL, 0) == -1, "NULL CDT accepted connect data");
    CHECK(scs_cdl_alloc(NULL, "X", "X", NULL) == NULL, "NULL CDL allocated a CDT");
    CHECK(scs_cdl_alloc_conid(NULL, SCS_CDT_MAKE_CONID(1), "X", "X", NULL) == NULL,
          "NULL CDL claimed a Con.ID");
    CHECK(scs_cdl_lookup(NULL, 1) == NULL, "NULL CDL resolved a CONID");
    CHECK(scs_cdl_deliver_message(NULL, 1, 0, "x", 1) == SCS_DELIVER_NO_CDT,
          "NULL CDL delivered a message");
    CHECK(scs_pb_first_cdt(NULL) == NULL, "NULL PB has a first CDT");
    CHECK(scs_cdt_next_on_pb(NULL) == NULL, "NULL CDT has a next");
    CHECK(scs_pb_cdt_count(NULL) == 0, "NULL PB carries connections");
    CHECK(scs_cdl_in_use_count(NULL) == 0, "NULL CDL has connections in use");

    /* SYSAP names longer than the 16-byte field are truncated, never overrun. */
    struct scs_cdt *t =
        scs_cdl_alloc(&cdl, "0123456789ABCDEFGHIJ", "0123456789ABCDEFGHIJ", NULL);
    CHECK(t != NULL, "allocation with an over-long SYSAP name failed");
    if (t != NULL) {
        CHECK(strlen(t->local_sysap) == SCS_CDT_SYSAP_NAME_LEN,
              "over-long SYSAP name was not truncated to the field width (got %zu)",
              strlen(t->local_sysap));
        CHECK(strcmp(t->local_sysap, "0123456789ABCDEF") == 0, "truncated name is '%s'",
              t->local_sysap);
    }
}

int main(void)
{
    test_cdl_init_sizing();
    test_conid_indexes_cdl_to_cdt();
    test_two_cdts_on_one_pb();
    test_packet_routed_by_destination_conid();
    test_vc_loss_notifies_every_connection_on_the_circuit();
    test_two_tier_allocation_and_exhaustion();
    test_shipped_wire_conids_are_valid_cdl_slots();
    test_rebind_and_null_safety();

    printf("test_scs_cdt: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
