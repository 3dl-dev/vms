/*
 * test_scs_config.c - SB / PB / PDT configuration-queue tests (vms-7be).
 *
 * Asserts the documented queue transitions of Roy G. Davis, *VAXcluster
 * Principles*, Digital Press 1993, ch. 2:
 *   - a new PB starts CLOSED and is queued to the PDT as formative   (pp. 2-11, 2-20)
 *   - the formation dialogue drives the VC state machine             (pp. 2-12..2-14)
 *   - on OPEN with an unknown node: SB into the configuration queue,
 *     PB dequeued from the PDT and queued to that SB                       (p. 2-21)
 *   - on OPEN with a known node that already has a PB: PB queued to the old
 *     SB, formative SB discarded, old SB NOT refreshed                     (p. 2-21)
 *   - THE NOTE: on OPEN with a known node that has NO PB left, the old SB IS
 *     refreshed from the formative SB before it is discarded (the depart/
 *     reboot/rejoin case)                                                  (p. 2-21)
 *   - a configuration queue holding several SBs with several PBs each
 *                                                              (pp. 2-17..2-19)
 *   - vms-22e: the anti-masquerade tests of the p. 2-21 footnote -- ID match /
 *     name mismatch, the converse, both matching with a Path Block queued and
 *     differing incarnations, and both matching with NO Path Block queued (the
 *     rebooted node, which is refreshed and admitted); plus the
 *     OVMX_NO_MASQUERADE_TESTS kill-switch bracketed both ways, and the rule
 *     that an input the local system never learned cannot convict a node
 *

 * Pure state: this test builds no frame and opens no socket.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scs_config.h"
#include "scs_credit.h"
#include "scs_depart.h"

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

/* Lab-shaped identities: an Ethernet port address (MAC) and the 48-bit SCS
 * System Address aa:00:04:00:<LE16(SCSSYSTEMID)>. Values are arbitrary test
 * data, not captured wire content. */
static void sysid_from_scssystemid(uint16_t id, uint8_t out[SCS_SYSTEM_ID_LEN])
{
    out[0] = 0xaa;
    out[1] = 0x00;
    out[2] = 0x04;
    out[3] = 0x00;
    out[4] = (uint8_t)(id & 0xff);
    out[5] = (uint8_t)((id >> 8) & 0xff);
}

static struct scs_sb_info make_info(uint16_t scssystemid, const char *node,
                                    uint64_t incarnation, uint16_t cpu_type)
{
    struct scs_sb_info info;
    memset(&info, 0, sizeof(info));
    sysid_from_scssystemid(scssystemid, info.system_id);
    info.node_name = node;
    info.incarnation = incarnation;
    info.cpu_type = cpu_type;
    info.hw_rev = 1;
    info.os_name = "VAX/VMS";
    info.os_version = 0x0703;
    return info;
}

/* p. 2-11 / p. 2-20: a freshly created PB is CLOSED and formative on its PDT. */
static void test_pb_created_closed_and_formative(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt;
    const uint8_t mac[6] = {0x08, 0x00, 0x2b, 0x11, 0x22, 0x33};

    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 1498);

    struct scs_pb *pb = scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET);
    CHECK(pb != NULL, "PB creation failed");
    CHECK(pb->vc_state == SCS_VC_CLOSED, "new PB VC state is %s, expected CLOSED",
          scs_vc_state_name(pb->vc_state));
    CHECK(pb->on_pdt == 1, "new PB is not queued to the PDT");
    CHECK(scs_pdt_formative_count(&pdt) == 1, "PDT formative queue holds %u, expected 1",
          scs_pdt_formative_count(&pdt));
    CHECK(scs_config_sb_count(&cfg) == 0, "configuration queue is not empty before OPEN");
    CHECK(memcmp(pb->remote_port_addr, mac, 6) == 0, "remote port address not recorded");
    CHECK(scs_config_find_pb(&cfg, &pdt, mac) == pb, "find_pb missed a formative PB");
}

/* pp. 2-12..2-14: CLOSED -> START SENT -> START RECEIVED -> OPEN, with the SB
 * built from the received START, then the p. 2-21 first-contact transition. */
static void test_formation_dialogue_and_first_contact(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt;
    const uint8_t mac[6] = {0x08, 0x00, 0x2b, 0xaa, 0xbb, 0xcc};

    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 1498);
    struct scs_pb *pb = scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET);

    scs_pb_set_vc_state(pb, SCS_VC_START_SENT);
    CHECK(pb->vc_state == SCS_VC_START_SENT, "state after sending START is %s",
          scs_vc_state_name(pb->vc_state));

    struct scs_sb_info info = make_info(1025, "VAX1", 0x1111111100000000ull, 7);
    struct scs_sb *formative = scs_pb_attach_formative_sb(&cfg, pb, &info);
    CHECK(formative != NULL, "formative SB not built from START");
    CHECK(formative->formative == 1, "SB built during formation is not marked formative");
    CHECK(pb->sb == formative, "formative SB address not placed in the PB");
    CHECK(scs_config_sb_count(&cfg) == 0, "formative SB must NOT be in the configuration queue");
    CHECK(strcmp(formative->node_name, "VAX1") == 0, "SB node name is '%s'", formative->node_name);
    CHECK(formative->incarnation == 0x1111111100000000ull, "SB incarnation not recorded");

    scs_pb_set_vc_state(pb, SCS_VC_START_RECEIVED);
    CHECK(pb->vc_state == SCS_VC_START_RECEIVED, "state after STACK is %s",
          scs_vc_state_name(pb->vc_state));

    enum scs_open_result r = scs_pb_open(&cfg, pb);
    CHECK(r == SCS_OPEN_NEW_SB, "first contact returned %d, expected SCS_OPEN_NEW_SB", (int)r);
    CHECK(pb->vc_state == SCS_VC_OPEN, "VC state after open is %s",
          scs_vc_state_name(pb->vc_state));
    CHECK(scs_pdt_formative_count(&pdt) == 0, "PB was not dequeued from the PDT");
    CHECK(scs_config_sb_count(&cfg) == 1, "configuration queue holds %u SBs, expected 1",
          scs_config_sb_count(&cfg));
    CHECK(formative->formative == 0, "SB is still marked formative after opening");
    CHECK(scs_sb_pb_count(formative) == 1, "SB holds %u PBs, expected 1",
          scs_sb_pb_count(formative));
    CHECK(scs_config_find_sb(&cfg, info.system_id) == formative, "SB not findable by System ID");
    CHECK(scs_config_find_pb(&cfg, &pdt, mac) == pb, "find_pb missed an OPEN PB");
}

/*
 * p. 2-19 Figure 2-13 / p. 2-21: a SECOND circuit to a node already in the
 * configuration queue. The formative PB is queued to the OLD SB and the
 * formative SB is discarded. Because another PB is already queued there, the
 * old SB is NOT refreshed -- its recorded incarnation must survive.
 */
static void test_second_circuit_no_refresh(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt1, pdt2;
    const uint8_t mac_a[6] = {0x08, 0x00, 0x2b, 0x00, 0x00, 0x01};
    const uint8_t mac_b[6] = {0x08, 0x00, 0x2b, 0x00, 0x00, 0x02};

    scs_config_init(&cfg);
    scs_pdt_init(&pdt1, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_pdt_init(&pdt2, SCS_PORT_TYPE_CI, 4096);

    struct scs_sb_info first = make_info(1026, "VAX2", 0xAAAAAAAAull, 7);
    first.hw_rev = 3;
    first.os_version = 0x0703;
    struct scs_pb *pb1 = scs_pb_create(&cfg, &pdt1, mac_a, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(&cfg, pb1, &first);
    CHECK(scs_pb_open(&cfg, pb1) == SCS_OPEN_NEW_SB, "first circuit did not create the SB");
    struct scs_sb *sb = pb1->sb;

    /* Same node (same 48-bit System ID) reached over a second local port.
     *
     * vms-22e: the SAME incarnation, deliberately. A node has exactly ONE
     * software incarnation number at a time (p. 2-16: it changes when the node
     * reboots), so two simultaneously-open circuits to one node necessarily
     * carry the same value -- and presenting a DIFFERENT one here is now the
     * p. 2-21 footnote masquerade failure, covered by
     * test_masquerade_incarnation_mismatch_abandons(). The "was it refreshed?"
     * question is therefore asked of hw_rev/os_version, fields the footnote
     * does not compare, so this test still proves the no-refresh rule without
     * depending on a state the architecture forbids. */
    struct scs_sb_info second = make_info(1026, "VAX2", 0xAAAAAAAAull, 7);
    second.hw_rev = 9;
    second.os_version = 0x0704;
    struct scs_pb *pb2 = scs_pb_create(&cfg, &pdt2, mac_b, SCS_PORT_TYPE_CI);
    scs_pb_attach_formative_sb(&cfg, pb2, &second);
    CHECK(scs_config_sb_count(&cfg) == 1, "formative SB leaked into the configuration queue");

    enum scs_open_result r = scs_pb_open(&cfg, pb2);
    CHECK(r == SCS_OPEN_EXISTING_SB, "second circuit returned %d, expected SCS_OPEN_EXISTING_SB",
          (int)r);
    CHECK(scs_config_sb_count(&cfg) == 1, "configuration queue holds %u SBs, expected 1",
          scs_config_sb_count(&cfg));
    CHECK(scs_sb_pb_count(sb) == 2, "SB holds %u PBs, expected 2 (two circuits, one node)",
          scs_sb_pb_count(sb));
    CHECK(pb2->sb == sb, "second PB was not re-pointed at the old SB");
    CHECK(sb->hw_rev == 3,
          "old SB was refreshed (hw_rev %u, expected 3) although another PB was"
          " queued to it",
          sb->hw_rev);
    CHECK(sb->os_version == 0x0703,
          "old SB was refreshed (os_version 0x%04x, expected 0x0703) although"
          " another PB was queued to it",
          sb->os_version);
    CHECK(scs_pdt_formative_count(&pdt2) == 0, "second PB was not dequeued from its PDT");
}

/*
 * THE NOTE, p. 2-21: the node departed (its only PB went away, the SB stayed in
 * the configuration queue per p. 2-17) and is now rebooting. When the new
 * formative PB opens against the old SB and NO other PB is queued there, the old
 * SB is REFRESHED from the formative SB before the formative SB is discarded.
 */
static void test_rejoin_refreshes_old_sb(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt;
    const uint8_t mac[6] = {0x08, 0x00, 0x2b, 0x0d, 0x0e, 0x0f};

    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 1498);

    struct scs_sb_info boot1 = make_info(1027, "VAX3", 0x1000ull, 7);
    boot1.hw_rev = 3;
    struct scs_pb *pb1 = scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(&cfg, pb1, &boot1);
    CHECK(scs_pb_open(&cfg, pb1) == SCS_OPEN_NEW_SB, "initial join did not create the SB");
    struct scs_sb *sb = pb1->sb;

    /* Departure: the only circuit closes. The SB stays (p. 2-17). */
    CHECK(scs_pb_close(&cfg, pb1) == SCS_PB_CLOSE_OK, "closing an open PB was refused");
    CHECK(scs_config_sb_count(&cfg) == 1, "SB was dropped when its last circuit closed");
    CHECK(scs_sb_pb_count(sb) == 0, "SB still holds %u PBs after departure",
          scs_sb_pb_count(sb));
    CHECK(sb->incarnation == 0x1000ull, "SB lost its incarnation on departure");

    /* Reboot: a new incarnation, a new formative PB and SB. */
    struct scs_sb_info boot2 = make_info(1027, "VAX3", 0x2000ull, 7);
    boot2.hw_rev = 4;
    boot2.os_version = 0x0704;
    struct scs_pb *pb2 = scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(&cfg, pb2, &boot2);
    struct scs_sb *formative = pb2->sb;
    CHECK(formative != sb, "rejoin reused the old SB instead of building a formative one");

    enum scs_open_result r = scs_pb_open(&cfg, pb2);
    CHECK(r == SCS_OPEN_EXISTING_REFRESHED,
          "rejoin returned %d, expected SCS_OPEN_EXISTING_REFRESHED", (int)r);
    CHECK(scs_config_sb_count(&cfg) == 1, "configuration queue holds %u SBs, expected 1",
          scs_config_sb_count(&cfg));
    CHECK(pb2->sb == sb, "rejoined PB is not queued to the OLD SB");
    CHECK(scs_sb_pb_count(sb) == 1, "old SB holds %u PBs, expected 1", scs_sb_pb_count(sb));
    CHECK(sb->incarnation == 0x2000ull,
          "old SB was NOT refreshed: incarnation is 0x%llx, expected 0x2000",
          (unsigned long long)sb->incarnation);
    CHECK(sb->hw_rev == 4, "old SB hardware revision was not refreshed (%u)", sb->hw_rev);
    CHECK(sb->os_version == 0x0704, "old SB OS version was not refreshed (0x%04x)",
          sb->os_version);
    CHECK(formative->in_use == 0, "formative SB was not discarded");
}

/*
 * pp. 2-17..2-19: a configuration queue holding several SBs, with several PBs
 * queued to some of them -- VAX_1's view in Figure 2-10 (its own SB plus one
 * per remote node; two PBs for the node it reaches over two interconnects).
 */
static void test_multi_sb_multi_pb_queue(void)
{
    struct scs_config cfg;
    struct scs_pdt ci, enet;
    scs_config_init(&cfg);
    scs_pdt_init(&ci, SCS_PORT_TYPE_CI, 4096);
    scs_pdt_init(&enet, SCS_PORT_TYPE_ETHERNET, 1498);

    /* The local node's own SB (p. 2-16: a node must maintain an SB describing
     * its own CPU and operating system; Figure 2-10 shows it in the queue). */
    struct scs_sb_info self = make_info(1030, "OVMX", 0x9000ull, 0);
    self.os_name = "OVMX";
    struct scs_sb *self_sb = scs_config_insert_sb(&cfg, &self);
    CHECK(self_sb != NULL, "self SB not inserted");
    CHECK(self_sb->formative == 0, "self SB must not be formative");
    CHECK(scs_config_insert_sb(&cfg, &self) == self_sb, "duplicate self SB inserted");

    struct {
        uint16_t sysid;
        const char *name;
        int circuits;
    } nodes[3] = {
        {1025, "VAX1", 2}, /* reachable over both interconnects */
        {1026, "VAX2", 1},
        {1027, "VAX3", 1},
    };

    uint8_t mac[6] = {0x08, 0x00, 0x2b, 0x00, 0x00, 0x00};
    for (int i = 0; i < 3; i++) {
        for (int c = 0; c < nodes[i].circuits; c++) {
            struct scs_pdt *pdt = (c == 0) ? &enet : &ci;
            mac[4] = (uint8_t)i;
            mac[5] = (uint8_t)c;
            struct scs_sb_info info = make_info(nodes[i].sysid, nodes[i].name,
                                                0x100ull + (uint64_t)i, 7);
            struct scs_pb *pb = scs_pb_create(&cfg, pdt, mac,
                                             (c == 0) ? SCS_PORT_TYPE_ETHERNET
                                                      : SCS_PORT_TYPE_CI);
            CHECK(pb != NULL, "PB pool exhausted building the queue");
            CHECK(scs_pb_attach_formative_sb(&cfg, pb, &info) != NULL,
                  "SB pool exhausted building the queue");
            enum scs_open_result r = scs_pb_open(&cfg, pb);
            CHECK(r == (c == 0 ? SCS_OPEN_NEW_SB : SCS_OPEN_EXISTING_SB),
                  "node %s circuit %d open returned %d", nodes[i].name, c, (int)r);
        }
    }

    CHECK(scs_config_sb_count(&cfg) == 4,
          "configuration queue holds %u SBs, expected 4 (self + 3 remote nodes)",
          scs_config_sb_count(&cfg));
    CHECK(scs_pdt_formative_count(&ci) == 0, "CI PDT still holds formative PBs");
    CHECK(scs_pdt_formative_count(&enet) == 0, "Ethernet PDT still holds formative PBs");

    for (int i = 0; i < 3; i++) {
        uint8_t id[SCS_SYSTEM_ID_LEN];
        sysid_from_scssystemid(nodes[i].sysid, id);
        struct scs_sb *sb = scs_config_find_sb(&cfg, id);
        CHECK(sb != NULL, "SB for %s missing from the configuration queue", nodes[i].name);
        CHECK(sb != NULL && scs_sb_pb_count(sb) == (unsigned)nodes[i].circuits,
              "SB for %s holds %u PBs, expected %d", nodes[i].name,
              scs_sb_pb_count(sb), nodes[i].circuits);
    }
    CHECK(scs_sb_pb_count(self_sb) == 0, "self SB unexpectedly holds PBs");
}

/*
 * scs_pb_learn_system_addr: OVMX learns the remote node's 48-bit SCS System
 * Address at port discovery (the HELLO src-logical field) rather than from a
 * START. It must create the formative SB on first sight, record exactly what the
 * wire said on every later sight, and never disturb an SB's queue membership.
 */
static void test_learn_system_addr_at_discovery(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt;
    const uint8_t mac[6] = {0x08, 0x00, 0x2b, 0x5a, 0x5b, 0x5c};
    uint8_t addr1[SCS_SYSTEM_ID_LEN], addr2[SCS_SYSTEM_ID_LEN];
    const uint8_t zeros[SCS_SYSTEM_ID_LEN] = {0, 0, 0, 0, 0, 0};

    sysid_from_scssystemid(1025, addr1);
    sysid_from_scssystemid(1099, addr2);

    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 1498);
    struct scs_pb *pb = scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET);

    CHECK(pb->sb == NULL, "a freshly discovered port must not have an SB yet");
    struct scs_sb *sb = scs_pb_learn_system_addr(&cfg, pb, addr1);
    CHECK(sb != NULL && pb->sb == sb, "learning the System Address built no SB");
    CHECK(sb != NULL && sb->formative == 1, "the discovery SB must be formative");
    CHECK(sb != NULL && memcmp(sb->system_id, addr1, SCS_SYSTEM_ID_LEN) == 0,
          "System Address not recorded");
    CHECK(scs_config_sb_count(&cfg) == 0, "discovery SB leaked into the configuration queue");

    /* Repeated sightings record what the wire said, including a zero address --
     * the per-peer field this replaced behaved exactly this way. */
    CHECK(scs_pb_learn_system_addr(&cfg, pb, addr2) == sb, "re-learn allocated a second SB");
    CHECK(memcmp(sb->system_id, addr2, SCS_SYSTEM_ID_LEN) == 0, "re-learn did not overwrite");
    scs_pb_learn_system_addr(&cfg, pb, zeros);
    CHECK(memcmp(sb->system_id, zeros, SCS_SYSTEM_ID_LEN) == 0,
          "a zero System Address was silently ignored");

    scs_pb_learn_system_addr(&cfg, pb, addr1);
    CHECK(scs_pb_open(&cfg, pb) == SCS_OPEN_NEW_SB, "open after discovery-learn failed");
    CHECK(scs_config_find_sb(&cfg, addr1) == sb, "SB not findable by the learned address");
    CHECK(scs_pb_learn_system_addr(NULL, pb, addr1) == NULL, "NULL cfg accepted");
    CHECK(scs_pb_learn_system_addr(&cfg, NULL, addr1) == NULL, "NULL pb accepted");
}

/* Defensive: NULL arguments and pool exhaustion must fail, never corrupt. */
static void test_limits_and_null_safety(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt;
    uint8_t mac[6] = {0x08, 0x00, 0x2b, 0x77, 0x00, 0x00};

    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 1498);

    CHECK(scs_pb_create(NULL, &pdt, mac, SCS_PORT_TYPE_ETHERNET) == NULL, "NULL cfg accepted");
    CHECK(scs_pb_create(&cfg, NULL, mac, SCS_PORT_TYPE_ETHERNET) == NULL, "NULL pdt accepted");
    CHECK(scs_config_find_sb(&cfg, NULL) == NULL, "NULL system id accepted");
    CHECK(scs_pb_open(&cfg, NULL) == SCS_OPEN_ERROR, "NULL PB accepted by open");

    struct scs_pb *pb = scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET);
    CHECK(scs_pb_open(&cfg, pb) == SCS_OPEN_ERROR, "open without an SB must fail");
    CHECK(scs_pb_close(&cfg, pb) == SCS_PB_CLOSE_OK, "closing a formative PB was refused");
    CHECK(scs_pb_close(&cfg, pb) == SCS_PB_CLOSE_NOTHING,
          "closing an already-closed PB must report that it did nothing");
    CHECK(scs_pb_close(&cfg, NULL) == SCS_PB_CLOSE_NOTHING, "NULL PB accepted by close");
    CHECK(scs_pdt_formative_count(&pdt) == 0, "closing a formative PB left it queued");

    for (unsigned i = 0; i < SCS_CONFIG_MAX_PB; i++) {
        mac[5] = (uint8_t)i;
        CHECK(scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET) != NULL,
              "PB pool exhausted early at %u", i);
    }
    mac[5] = 0xff;
    CHECK(scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET) == NULL,
          "PB pool overflowed past SCS_CONFIG_MAX_PB");
}

/*
 * CONFIG_SYS / CONFIG_PATH (vms-398, p. 2-47). Builds a System Block with
 * THREE open Path Blocks (three interconnects to one node -- an OVMX test
 * scenario per scs_config.h's design-choice note, not a production shape) and
 * walks CONFIG_SYS -> repeated CONFIG_PATH, checking every PB is reached
 * exactly once and the queue-position fields (sb/next/on_formative_queue)
 * agree with what scs_config.c itself queued.
 */
static void test_config_sys_and_path_walk_every_pb_once(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt_a, pdt_b, pdt_c;
    scs_config_init(&cfg);
    scs_pdt_init(&pdt_a, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_pdt_init(&pdt_b, SCS_PORT_TYPE_CI, 4096);
    scs_pdt_init(&pdt_c, SCS_PORT_TYPE_CI_HSC, 4096);

    struct scs_sb_info info = make_info(1040, "VAX9", 0x424242ull, 7);
    uint8_t sysid[SCS_SYSTEM_ID_LEN];
    sysid_from_scssystemid(1040, sysid);

    const uint8_t mac_a[6] = {0x08, 0x00, 0x2b, 0x40, 0x00, 0x01};
    const uint8_t mac_b[6] = {0x08, 0x00, 0x2b, 0x40, 0x00, 0x02};
    const uint8_t mac_c[6] = {0x08, 0x00, 0x2b, 0x40, 0x00, 0x03};

    struct scs_pb *pb_a = scs_pb_create(&cfg, &pdt_a, mac_a, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(&cfg, pb_a, &info);
    CHECK(scs_pb_open(&cfg, pb_a) == SCS_OPEN_NEW_SB, "first circuit did not create the SB");
    struct scs_sb *sb = pb_a->sb;

    struct scs_pb *pb_b = scs_pb_create(&cfg, &pdt_b, mac_b, SCS_PORT_TYPE_CI);
    scs_pb_attach_formative_sb(&cfg, pb_b, &info);
    CHECK(scs_pb_open(&cfg, pb_b) == SCS_OPEN_EXISTING_SB, "second circuit did not join the old SB");

    struct scs_pb *pb_c = scs_pb_create(&cfg, &pdt_c, mac_c, SCS_PORT_TYPE_CI_HSC);
    scs_pb_attach_formative_sb(&cfg, pb_c, &info);
    CHECK(scs_pb_open(&cfg, pb_c) == SCS_OPEN_EXISTING_SB, "third circuit did not join the old SB");

    /* Remote port STATE is written straight into the Path Block by its producer
     * (src/vmsscs/scsd.c ps_channel_up() does exactly this assignment; there is
     * no setter function). Give the three paths three DIFFERENT states so that
     * CONFIG_PATH's copy of the field cannot be satisfied by any constant. */
    pb_a->remote_port_state = SCS_PORT_STATE_ENABLED;
    pb_b->remote_port_state = SCS_PORT_STATE_MAINT_ENABLED;
    pb_c->remote_port_state = SCS_PORT_STATE_UNKNOWN;

    /* --- CONFIG_SYS --- */
    struct scs_config_sys_info sysinfo;
    CHECK(scs_config_sys(&cfg, sysid, &sysinfo) == 1, "CONFIG_SYS did not find the SB");
    CHECK(memcmp(sysinfo.system_id, sysid, SCS_SYSTEM_ID_LEN) == 0,
          "CONFIG_SYS echoed the wrong System ID");
    CHECK(sysinfo.first_pb == sb->pb_head, "CONFIG_SYS first_pb != sb->pb_head");
    CHECK((sysinfo.have & SCS_CONFIG_SYS_HAVE_MAX_DATAGRAM) == 0,
          "HAVE_MAX_DATAGRAM must never be set (no such SB field exists)");
    CHECK(sysinfo.max_datagram_size == 0, "max_datagram_size must be 0, not invented");
    CHECK((sysinfo.have & SCS_CONFIG_SYS_HAVE_MAX_MESSAGE) == 0,
          "HAVE_MAX_MESSAGE must never be set (no such SB field exists)");
    CHECK((sysinfo.have & SCS_CONFIG_SYS_HAVE_SOFTWARE_TYPE) != 0,
          "software type should be known: this test's SB was built via attach_formative_sb");
    CHECK(strcmp(sysinfo.software_type, "VAX/VMS") == 0, "software_type is '%s'",
          sysinfo.software_type);
    CHECK((sysinfo.have & SCS_CONFIG_SYS_HAVE_SOFTWARE_VERSION) != 0,
          "software version should be known");
    CHECK(sysinfo.software_version == 0x0703, "software_version is 0x%04x",
          sysinfo.software_version);
    CHECK((sysinfo.have & SCS_CONFIG_SYS_HAVE_NODE_NAME) != 0, "node name should be known");
    CHECK(strcmp(sysinfo.node_name, "VAX9") == 0, "node_name is '%s'", sysinfo.node_name);

    /* --- repeated CONFIG_PATH: walk from first_pb, must reach all 3 exactly once --- */
    int seen_a = 0, seen_b = 0, seen_c = 0, total = 0;
    struct scs_pb *cursor = sysinfo.first_pb;
    struct scs_config_path_info pinfo;
    while (cursor != NULL) {
        total++;
        CHECK(total <= 3, "CONFIG_PATH walk visited more PBs than were queued (loop?)");
        if (total > 3) {
            break; /* guard against an infinite loop corrupting the test run */
        }
        CHECK(scs_config_path(cursor, &pinfo) == 1, "CONFIG_PATH failed on a live PB");
        CHECK(pinfo.vc_state == SCS_VC_OPEN, "CONFIG_PATH vc_state is %s, expected OPEN",
              scs_vc_state_name(pinfo.vc_state));
        CHECK(pinfo.sb == sb, "CONFIG_PATH sb identifier does not match the SB");
        CHECK(pinfo.on_formative_queue == 0, "an OPEN PB must not read as on the formative queue");

        /* p. 2-47 "the type and state of the remote port" -- all THREE remote
         * port characteristics are asserted per PB (type, state, address), each
         * with a value unique to that PB, so no constant or dropped copy in
         * scs_config_path() can satisfy them. */
        if (cursor == pb_a) {
            seen_a++;
            CHECK(pinfo.remote_port_type == SCS_PORT_TYPE_ETHERNET, "pb_a port type wrong");
            CHECK(pinfo.remote_port_state == SCS_PORT_STATE_ENABLED,
                  "pb_a port state is %d, expected ENABLED", (int)pinfo.remote_port_state);
            CHECK(memcmp(pinfo.remote_port_addr, mac_a, SCS_PORT_ADDR_LEN) == 0,
                  "pb_a remote port address not reported by CONFIG_PATH");
        } else if (cursor == pb_b) {
            seen_b++;
            CHECK(pinfo.remote_port_type == SCS_PORT_TYPE_CI, "pb_b port type wrong");
            CHECK(pinfo.remote_port_state == SCS_PORT_STATE_MAINT_ENABLED,
                  "pb_b port state is %d, expected MAINT_ENABLED", (int)pinfo.remote_port_state);
            CHECK(memcmp(pinfo.remote_port_addr, mac_b, SCS_PORT_ADDR_LEN) == 0,
                  "pb_b remote port address not reported by CONFIG_PATH");
        } else if (cursor == pb_c) {
            seen_c++;
            CHECK(pinfo.remote_port_type == SCS_PORT_TYPE_CI_HSC, "pb_c port type wrong");
            CHECK(pinfo.remote_port_state == SCS_PORT_STATE_UNKNOWN,
                  "pb_c port state is %d, expected UNKNOWN", (int)pinfo.remote_port_state);
            CHECK(memcmp(pinfo.remote_port_addr, mac_c, SCS_PORT_ADDR_LEN) == 0,
                  "pb_c remote port address not reported by CONFIG_PATH");
        } else {
            CHECK(0, "CONFIG_PATH walk reached a PB that was never queued to this SB");
        }
        cursor = pinfo.next;
    }
    CHECK(total == 3, "CONFIG_PATH walk visited %d PBs, expected 3", total);
    CHECK(seen_a == 1 && seen_b == 1 && seen_c == 1,
          "every PB must be reached EXACTLY once (a=%d b=%d c=%d)", seen_a, seen_b, seen_c);

    /* --- CONNECT's "no circuit named" path: CONFIG_SYS + queue scan ---
     * With every circuit OPEN the answer is exactly the first PB in the queue.
     * That the scan actually SKIPS non-OPEN circuits is a separate property and
     * is tested in test_select_vc_skips_a_circuit_that_is_not_open(); this
     * assertion alone would not detect an unconditional "take the head". */
    struct scs_pb *chosen = scs_config_select_vc(&cfg, sysid);
    CHECK(chosen == sb->pb_head,
          "with all circuits OPEN, select_vc must return the first PB in the queue");
    CHECK(chosen != NULL && chosen->vc_state == SCS_VC_OPEN,
          "select_vc must only choose an OPEN circuit");

    /* Unknown System ID / NULL safety. */
    uint8_t unknown[SCS_SYSTEM_ID_LEN];
    sysid_from_scssystemid(9999, unknown);
    struct scs_config_sys_info miss;
    memset(&miss, 0xAA, sizeof(miss));
    CHECK(scs_config_sys(&cfg, unknown, &miss) == 0, "CONFIG_SYS found a nonexistent System ID");
    CHECK(miss.have == 0 && miss.first_pb == NULL, "CONFIG_SYS miss did not zero *out");
    CHECK(scs_config_sys(NULL, sysid, &sysinfo) == 0, "CONFIG_SYS accepted NULL cfg");
    CHECK(scs_config_sys(&cfg, NULL, &sysinfo) == 0, "CONFIG_SYS accepted NULL system_id");
    CHECK(scs_config_sys(&cfg, sysid, NULL) == 0, "CONFIG_SYS accepted NULL out");
    CHECK(scs_config_select_vc(&cfg, unknown) == NULL, "select_vc found a circuit for an unknown node");
    CHECK(scs_config_select_vc(NULL, sysid) == NULL, "select_vc accepted NULL cfg");

    CHECK(scs_config_path(NULL, &pinfo) == 0, "CONFIG_PATH accepted NULL pb");
    CHECK(scs_config_path(pb_a, NULL) == 0, "CONFIG_PATH accepted NULL out");
}

/*
 * CONFIG_PATH on a FORMATIVE (still-forming) PB: on_formative_queue must read
 * 1, and the SB identifier returned is the formative SB (p. 2-20), since
 * CONFIG_PATH is defined over "a" Path Block, not only open ones.
 */
static void test_config_path_on_formative_pb(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt;
    const uint8_t mac[6] = {0x08, 0x00, 0x2b, 0x50, 0x00, 0x01};

    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 1498);
    struct scs_pb *pb = scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET);
    scs_pb_set_vc_state(pb, SCS_VC_START_SENT);

    struct scs_config_path_info pinfo;
    CHECK(scs_config_path(pb, &pinfo) == 1, "CONFIG_PATH failed on a formative PB");
    CHECK(pinfo.vc_state == SCS_VC_START_SENT, "formative PB vc_state is %s",
          scs_vc_state_name(pinfo.vc_state));
    CHECK(pinfo.on_formative_queue == 1, "PB queued to a PDT must read on_formative_queue == 1");
    CHECK(pinfo.sb == NULL, "a PB with no SB yet must report sb == NULL");
    CHECK(pinfo.next == NULL, "sole formative PB on this PDT has no next");

    /* A closed (freed) PB is no longer a valid query target. */
    CHECK(scs_pb_close(&cfg, pb) == SCS_PB_CLOSE_OK, "closing the query target was refused");
    CHECK(scs_config_path(pb, &pinfo) == 0, "CONFIG_PATH must refuse a closed/freed PB");
}

/*
 * vms-398 CONSTRAINT: "report what the structure holds; do NOT invent a value."
 *
 * The test above queries an SB built by scs_pb_attach_formative_sb(), which the
 * live daemon never calls -- so it only ever asks about a shape SCSD cannot
 * produce. THIS test asks about the shape SCSD DOES produce: an SB created by
 * scs_pb_learn_system_addr() at port discovery (the only SB producer on the
 * daemon's peer path, src/vmsscs/scsd.c ps_learn_sys_addr) and then opened.
 * Such an SB carries a System Address and NOTHING ELSE, so CONFIG_SYS must
 * report software type, software version and node name as ABSENT -- HAVE bits
 * clear, fields empty/zero. An implementation that claimed to know them anyway
 * would be inventing values, and this is what catches it.
 */
static void test_config_sys_does_not_invent_unknown_fields(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt;
    const uint8_t mac[6] = {0x08, 0x00, 0x2b, 0x60, 0x00, 0x01};
    uint8_t sysid[SCS_SYSTEM_ID_LEN];

    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 1498);
    sysid_from_scssystemid(1329, sysid);

    /* The production path, in order: discover the port, learn the System
     * Address off the HELLO, run the circuit to OPEN. No START description of
     * the node is ever applied, because SCSD never applies one. */
    struct scs_pb *pb = scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET);
    CHECK(scs_pb_learn_system_addr(&cfg, pb, sysid) != NULL, "learn_system_addr failed");
    CHECK(scs_pb_open(&cfg, pb) == SCS_OPEN_NEW_SB, "the discovered node did not create an SB");
    CHECK(pb->sb != NULL && pb->sb->os_name[0] == '\0' && pb->sb->os_version == 0 &&
              pb->sb->node_name[0] == '\0',
          "the SB SCSD builds is no longer software-description-free -- this test"
          " no longer exercises the unknown-field case, fix it rather than delete it");

    struct scs_config_sys_info si;
    memset(&si, 0xAA, sizeof(si)); /* poison: every reported field must be written */
    CHECK(scs_config_sys(&cfg, sysid, &si) == 1, "CONFIG_SYS did not find the discovered node");

    /* What IS known is reported. */
    CHECK(memcmp(si.system_id, sysid, SCS_SYSTEM_ID_LEN) == 0,
          "CONFIG_SYS did not echo the System ID of a discovered node");
    CHECK(si.first_pb == pb, "CONFIG_SYS first_pb is not the node's only Path Block");

    /* What is NOT known is reported as not known -- never guessed. */
    CHECK((si.have & SCS_CONFIG_SYS_HAVE_SOFTWARE_TYPE) == 0,
          "CONFIG_SYS claimed to know the software type of a node that never"
          " described itself (have=0x%x)", si.have);
    CHECK(si.software_type[0] == '\0', "CONFIG_SYS invented a software type: '%s'",
          si.software_type);
    CHECK((si.have & SCS_CONFIG_SYS_HAVE_SOFTWARE_VERSION) == 0,
          "CONFIG_SYS claimed to know the software version of a node that never"
          " described itself (have=0x%x)", si.have);
    CHECK(si.software_version == 0, "CONFIG_SYS invented a software version: 0x%04x",
          si.software_version);
    CHECK((si.have & SCS_CONFIG_SYS_HAVE_NODE_NAME) == 0,
          "CONFIG_SYS claimed to know the SCS Node Name of a node that never"
          " described itself (have=0x%x)", si.have);
    CHECK(si.node_name[0] == '\0', "CONFIG_SYS invented a node name: '%s'", si.node_name);

    /* The two fields no SB has ever held are never claimed either. */
    CHECK((si.have & SCS_CONFIG_SYS_HAVE_MAX_DATAGRAM) == 0 && si.max_datagram_size == 0,
          "CONFIG_SYS invented a maximum datagram size");
    CHECK((si.have & SCS_CONFIG_SYS_HAVE_MAX_MESSAGE) == 0 && si.max_message_size == 0,
          "CONFIG_SYS invented a maximum message size");

    /* Taken together: for the configuration SCSD actually builds, CONFIG_SYS
     * asserts nothing beyond the node's identity and its Path Block queue. */
    CHECK(si.have == 0, "CONFIG_SYS have-mask is 0x%x for a node whose SB holds only"
          " a System Address; expected 0", si.have);
}

/*
 * p. 2-47: CONNECT "examines each Path Block in turn until it finds one whose
 * virtual circuit is OPEN". The head of the queue being OPEN is the easy case;
 * this is the case that makes the rule a rule. A node with two Path Blocks
 * whose FIRST circuit is not OPEN must be reached over the second one.
 */
static void test_select_vc_skips_a_circuit_that_is_not_open(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt_a, pdt_b;
    const uint8_t mac_a[6] = {0x08, 0x00, 0x2b, 0x70, 0x00, 0x01};
    const uint8_t mac_b[6] = {0x08, 0x00, 0x2b, 0x70, 0x00, 0x02};
    uint8_t sysid[SCS_SYSTEM_ID_LEN];

    scs_config_init(&cfg);
    scs_pdt_init(&pdt_a, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_pdt_init(&pdt_b, SCS_PORT_TYPE_ETHERNET, 1498);
    sysid_from_scssystemid(1041, sysid);
    struct scs_sb_info info = make_info(1041, "VAX8", 0x515151ull, 7);

    struct scs_pb *first_opened = scs_pb_create(&cfg, &pdt_a, mac_a, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(&cfg, first_opened, &info);
    CHECK(scs_pb_open(&cfg, first_opened) == SCS_OPEN_NEW_SB, "first circuit was not NEW_SB");
    struct scs_sb *sb = first_opened->sb;

    struct scs_pb *head = scs_pb_create(&cfg, &pdt_b, mac_b, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(&cfg, head, &info);
    CHECK(scs_pb_open(&cfg, head) == SCS_OPEN_EXISTING_SB, "second circuit did not join the SB");

    /* PBs queue at the HEAD, so the second circuit opened is the one CONNECT
     * examines first. Take that circuit down while its Path Block stays queued
     * (scs_pb_close would remove it and there would be nothing to skip). */
    CHECK(sb != NULL && sb->pb_head == head,
          "the second circuit is not at the head of the queue -- this test would"
          " pass without ever exercising the skip");
    scs_pb_set_vc_state(head, SCS_VC_CLOSED);

    struct scs_pb *chosen = scs_config_select_vc(&cfg, sysid);
    CHECK(chosen == first_opened,
          "select_vc returned the CLOSED circuit at the head of the queue instead"
          " of scanning on to the OPEN one (p. 2-47)");

    /* The same skip for every non-OPEN stage of formation, not just CLOSED. */
    scs_pb_set_vc_state(head, SCS_VC_START_SENT);
    CHECK(scs_config_select_vc(&cfg, sysid) == first_opened,
          "select_vc accepted a START-SENT circuit as OPEN");
    scs_pb_set_vc_state(head, SCS_VC_START_RECEIVED);
    CHECK(scs_config_select_vc(&cfg, sysid) == first_opened,
          "select_vc accepted a START-RECEIVED circuit as OPEN");

    /* And with NO circuit open, CONNECT gets nothing -- it must not fall back to
     * "any Path Block will do". */
    scs_pb_set_vc_state(first_opened, SCS_VC_CLOSED);
    CHECK(scs_config_select_vc(&cfg, sysid) == NULL,
          "select_vc chose a circuit though none of the node's circuits is OPEN");

    /* Reopening the head makes it the answer again: the choice tracks circuit
     * state, not queue position or allocation order. */
    scs_pb_set_vc_state(head, SCS_VC_OPEN);
    CHECK(scs_config_select_vc(&cfg, sysid) == head,
          "select_vc did not follow the circuit back to OPEN");
}

/* ==========================================================================
 * vms-17f / vms-228 / vms-61b -- ORDERED TEARDOWN ON DEPARTURE.
 *
 * These cases are about the SEQUENCE, not about any one structure: p. 2-28
 * requires the SYSAPs on a circuit to be notified BEFORE the circuit goes away,
 * p. 2-43/2-45 require the connection's port buffers and suspended sends to be
 * dealt with as it is released, and p. 2-17 requires the System Block to
 * SURVIVE. Every one of those used to be a sentence in a comment; each is now
 * asserted, and the ordering one is asserted by the production refusal rather
 * than by the test being careful.
 * ========================================================================== */

/* What a SYSAP's VC-loss error handler saw when it ran (p. 2-28). */
struct vcloss_witness {
    int      calls;
    int      pb_was_still_in_use;
    int      cdt_was_still_on_the_pb;
    unsigned pb_cdt_count_at_notify;
};

static void vcloss_observer(struct scs_cdt *cdt, void *ctx)
{
    struct vcloss_witness *w = (struct vcloss_witness *)ctx;
    w->calls++;
    if (cdt->pb != NULL) {
        w->pb_was_still_in_use = cdt->pb->in_use;
        w->pb_cdt_count_at_notify = scs_pb_cdt_count(cdt->pb);
        for (const struct scs_cdt *c = scs_pb_first_cdt(cdt->pb); c != NULL;
             c = scs_cdt_next_on_pb(c)) {
            if (c == cdt) {
                w->cdt_was_still_on_the_pb = 1;
            }
        }
    }
}

/*
 * THE ORDERING, ENFORCED. A Path Block with connections still queued to it is
 * NOT closed -- scs_pb_close refuses, and refuses without changing anything.
 * This is the vms-228 defect: the old code memset the PB regardless, leaving
 * every CDT on it pointing at a recycled structure, and the only thing stopping
 * that was a comment telling the caller to go first.
 */
static void test_pb_close_refuses_while_connections_are_queued(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt;
    static struct scs_cdl cdl;
    const uint8_t mac[6] = {0x08, 0x00, 0x2b, 0x17, 0xf0, 0x01};

    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_cdl_init(&cdl);

    struct scs_sb_info info = make_info(1329, "VAX9", 0x11ull, 7);
    struct scs_pb *pb = scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(&cfg, pb, &info);
    CHECK(scs_pb_open(&cfg, pb) == SCS_OPEN_NEW_SB, "setup: first join was not NEW_SB");
    struct scs_sb *sb = pb->sb;

    struct scs_cdt *cdt = scs_cdl_alloc(&cdl, "VMS$VAXcluster", "VMS$VAXcluster", pb);
    CHECK(cdt != NULL, "setup: no CDT");
    CHECK(scs_pb_cdt_count(pb) == 1, "setup: the CDT is not on the circuit");

    CHECK(scs_pb_close(&cfg, pb) == SCS_PB_CLOSE_CONNECTIONS_QUEUED,
          "scs_pb_close destroyed a Path Block that still carried a connection");
    /* Refused means UNCHANGED, not half-done. */
    CHECK(pb->in_use == 1, "the refused close freed the Path Block anyway");
    CHECK(pb->sb == sb, "the refused close detached the Path Block from its SB");
    CHECK(scs_sb_pb_count(sb) == 1, "the refused close dequeued the PB from its SB");
    CHECK(scs_pb_cdt_count(pb) == 1, "the refused close touched the connection queue");
    CHECK(cdt->pb == pb, "the refused close left the CDT pointing somewhere else");

    /* Release the connection and the same call now succeeds -- so the refusal is
     * about the queue, not about this PB being unclosable. */
    scs_cdl_release(&cdl, cdt);
    CHECK(scs_pb_close(&cfg, pb) == SCS_PB_CLOSE_OK,
          "an empty circuit was still refused after its connection was released");
    CHECK(scs_config_sb_count(&cfg) == 1, "the System Block did not survive (p. 2-17)");
}

/*
 * THE FULL p. 2-28 SEQUENCE through scs_pb_depart(): the SYSAP error handler
 * runs while its connection and the circuit are STILL INTACT (that is the whole
 * point of notifying first), then the Credit Wait is abandoned (p. 2-45), the
 * MFREEQ share goes back to the port (p. 2-43, the vms-61b defect), the CDT is
 * released but stays in its CDL slot (p. 2-30), and only then is the Path Block
 * closed -- with its System Block left in the configuration queue (p. 2-17).
 */
static void test_depart_runs_the_p228_sequence_in_order(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt;
    static struct scs_cdl cdl;
    const uint8_t mac[6] = {0x08, 0x00, 0x2b, 0x17, 0xf0, 0x02};

    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_cdl_init(&cdl);
    scs_credit_reset_switch_cache();

    struct scs_sb_info info = make_info(1329, "VAX9", 0x11ull, 7);
    struct scs_pb *pb = scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(&cfg, pb, &info);
    CHECK(scs_pb_open(&cfg, pb) == SCS_OPEN_NEW_SB, "setup: first join was not NEW_SB");

    struct scs_cdt *cdt = scs_cdl_alloc(&cdl, "VMS$VAXcluster", "VMS$VAXcluster", pb);
    CHECK(cdt != NULL, "setup: no CDT");
    uint32_t conid = cdt->local_conid;

    struct vcloss_witness witness;
    memset(&witness, 0, sizeof(witness));
    scs_cdt_set_handlers(cdt, NULL, NULL, vcloss_observer, &witness);

    /* p. 2-43: forming the connection contributed 10 buffers to the port's
     * MFREEQ. That is the depth that must come back. */
    CHECK(scs_credit_extend(cdt, 10, 5) == 0, "setup: extend failed");
    CHECK(pdt.mfreeq_count == 10, "setup: MFREEQ depth is %u, expected 10", pdt.mfreeq_count);

    /* p. 2-45: with no Send Credits, two sends are suspended in a Credit Wait. */
    struct scs_credit_waiter w1, w2;
    memset(&w1, 0, sizeof(w1));
    memset(&w2, 0, sizeof(w2));
    CHECK(scs_credit_send_or_wait(cdt, &w1) == SCS_CREDIT_WAIT, "setup: w1 did not suspend");
    CHECK(scs_credit_send_or_wait(cdt, &w2) == SCS_CREDIT_WAIT, "setup: w2 did not suspend");
    CHECK(scs_credit_wait_depth(cdt) == 2, "setup: Credit Wait depth is %u, expected 2",
          scs_credit_wait_depth(cdt));

    struct scs_depart_stats st;
    enum scs_pb_close_result res = scs_pb_depart(&cdl, &cfg, pb, &st);

    CHECK(res == SCS_PB_CLOSE_OK, "departure did not close the Path Block (result %d)", (int)res);

    /* 1. The notification happened, and it happened FIRST -- the handler saw its
     *    own CDT still queued to a Path Block that was still in use. */
    CHECK(witness.calls == 1, "the VC-loss handler ran %d times, expected 1", witness.calls);
    CHECK(witness.pb_was_still_in_use == 1,
          "the SYSAP was notified AFTER its Path Block was destroyed (p. 2-28 order)");
    CHECK(witness.cdt_was_still_on_the_pb == 1,
          "the SYSAP was notified after its connection had been dequeued");
    CHECK(witness.pb_cdt_count_at_notify == 1,
          "the circuit held %u connections at notify time, expected 1",
          witness.pb_cdt_count_at_notify);
    CHECK(st.connections_lost == 1, "reported %u connections lost, expected 1",
          st.connections_lost);
    CHECK(st.handlers_notified == 1, "reported %u handlers, expected 1", st.handlers_notified);

    /* 2. p. 2-45: the waiters are gone, and gone WITHOUT being resumed. */
    CHECK(st.waiters_flushed == 2, "flushed %u credit waiters, expected 2", st.waiters_flushed);
    CHECK(w1.resumed == 0 && w2.resumed == 0,
          "a Credit Wait was RESUMED by a teardown -- the connection is broken and"
          " will never grant credit");
    CHECK(w1.queued == 0 && w2.queued == 0, "a waiter is still linked to a released CDT");

    /* 3. p. 2-43 / vms-61b: the port got its buffers back. */
    CHECK(st.mfreeq_reclaimed == 10, "reclaimed %u MFREEQ buffers, expected 10",
          st.mfreeq_reclaimed);
    CHECK(pdt.mfreeq_count == 0,
          "the port MFREEQ still holds %u buffers from a connection that no longer"
          " exists -- this is the vms-61b leak", pdt.mfreeq_count);

    /* 4. p. 2-30: released, not deallocated -- the CDL slot is reusable. */
    CHECK(scs_cdl_in_use_count(&cdl) == 0, "a CDT is still in use after teardown");
    struct scs_cdt *reused = scs_cdl_alloc_conid(&cdl, conid, "SCS$DIRECTORY", "SCS$DIRECTORY",
                                                 NULL);
    CHECK(reused != NULL, "the released CDT's CDL slot cannot be reused (p. 2-30)");

    /* 5. p. 2-17: the System Block outlives the circuit. That is the premise of
     *    the p. 2-21 rejoin REFRESH. */
    CHECK(scs_config_sb_count(&cfg) == 1, "the System Block was dropped with the circuit");
    CHECK(scs_pdt_formative_count(&pdt) == 0, "the closed PB was left on the PDT");
}

/*
 * The whole point, at module level: departure THEN return gives the p. 2-21
 * REFRESH, and it is the departure that makes the difference. The control is
 * the same sequence with no departure in it.
 */
static void test_depart_is_what_makes_the_rejoin_refresh(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt;
    const uint8_t mac[6] = {0x08, 0x00, 0x2b, 0x17, 0xf0, 0x03};

    /* CONTROL: no departure. The second circuit finds the old SB still holding a
     * Path Block, so the Note does not apply and nothing is refreshed.
     *
     * vms-22e: the second circuit MUST present the SAME 64-bit incarnation as
     * the first. A node that never left has not rebooted, so its incarnation has
     * not changed -- and with a Path Block still queued to the old SB, the
     * p. 2-21 footnote requires the incarnations to match on pain of abandoned
     * formation. This control originally presented 0x2000 here, which under the
     * footnote rule is a MASQUERADER, not a second port; it was a fixture that
     * had never been checked against the rule because the rule did not exist
     * yet. The distinguishing signal moved to cpu_type instead, which the
     * p. 2-21 Note refresh copies (scs_sb_apply_info) and no footnote test
     * inspects -- so "nothing was refreshed" is still asserted, non-vacuously. */
    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 1498);
    struct scs_sb_info boot1 = make_info(1329, "VAX9", 0x1000ull, 7);
    struct scs_pb *a = scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(&cfg, a, &boot1);
    CHECK(scs_pb_open(&cfg, a) == SCS_OPEN_NEW_SB, "control: first join was not NEW_SB");
    struct scs_sb_info second_port = make_info(1329, "VAX9", 0x1000ull, 9);
    struct scs_pb *b = scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(&cfg, b, &second_port);
    CHECK(scs_pb_open(&cfg, b) == SCS_OPEN_EXISTING_SB,
          "control: a second circuit to a node that never left took the REFRESH");
    CHECK(scs_config_find_sb(&cfg, boot1.system_id)->cpu_type == 7,
          "control: the old SB was refreshed although the node never departed"
          " (cpu_type is now %u, the formative SB's)",
          scs_config_find_sb(&cfg, boot1.system_id)->cpu_type);

    /* And the rebooted-incarnation shape the control used to carry IS now an
     * abandonment -- kept as a live assertion so the two rules stay wired to
     * each other rather than to a comment. */
    struct scs_sb_info impostor = make_info(1329, "VAX9", 0x2000ull, 7);
    struct scs_pb *c = scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(&cfg, c, &impostor);
    CHECK(scs_pb_open(&cfg, c) == SCS_OPEN_ABANDONED_MASQUERADE,
          "a changed incarnation with Path Blocks still queued was admitted --"
          " the p. 2-21 footnote incarnation test did not fire");

    struct scs_sb_info boot2 = make_info(1329, "VAX9", 0x2000ull, 7);

    /* THE CASE: same node, but it departs first. */
    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 1498);
    struct scs_pb *first = scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(&cfg, first, &boot1);
    CHECK(scs_pb_open(&cfg, first) == SCS_OPEN_NEW_SB, "first join was not NEW_SB");
    CHECK(scs_pb_depart(NULL, &cfg, first, NULL) == SCS_PB_CLOSE_OK,
          "departure of a connectionless circuit was not clean");

    struct scs_pb *back = scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(&cfg, back, &boot2);
    CHECK(scs_pb_open(&cfg, back) == SCS_OPEN_EXISTING_REFRESHED,
          "the returning node did not take the p. 2-21 REFRESH");
    CHECK(scs_config_sb_count(&cfg) == 1, "the rejoin duplicated the System Block");
    CHECK(scs_config_find_sb(&cfg, boot1.system_id)->incarnation == 0x2000ull,
          "the old SB was not refreshed from the formative SB");
}

/*
 * THE KILL SWITCH, RUN (guardrail 23). OVMX_NO_PEER_DEPART=1 must make
 * scs_pb_depart a complete no-op -- and the bracketing control either side of it
 * is what shows the switch is what changed the answer, not the test's setup.
 */
static void test_depart_kill_switch(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt;
    static struct scs_cdl cdl;
    const uint8_t mac[6] = {0x08, 0x00, 0x2b, 0x17, 0xf0, 0x04};

    unsetenv("OVMX_NO_PEER_DEPART");
    CHECK(scs_depart_enabled() == 1, "the departure sweep is not on by default");
    CHECK(scs_depart_listen_timeout_ms() == SCS_DEPART_LISTEN_TIMEOUT_DEFAULT_MS,
          "the default listen timeout is not SCS_DEPART_LISTEN_TIMEOUT_DEFAULT_MS");

    /* The measured bounds the default sits between; a change that moved it into
     * either observed population would red here. See scs_depart.h. */
    CHECK(SCS_DEPART_LISTEN_TIMEOUT_DEFAULT_MS > SCS_DEPART_HEALTHY_SILENCE_MAX_MS,
          "the listen timeout is at or below the longest silence measured on a"
          " HEALTHY link -- healthy peers would be declared departed");
    CHECK(SCS_DEPART_LISTEN_TIMEOUT_DEFAULT_MS < SCS_DEPART_OBSERVED_DEPARTURE_MS,
          "the listen timeout is above the silence of a peer that really did"
          " depart -- a real departure would never be detected");

    setenv("OVMX_NO_PEER_DEPART", "1", 1);
    CHECK(scs_depart_enabled() == 0, "OVMX_NO_PEER_DEPART=1 did not disable the sweep");

    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_cdl_init(&cdl);
    struct scs_sb_info info = make_info(1329, "VAX9", 0x11ull, 7);
    struct scs_pb *pb = scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(&cfg, pb, &info);
    CHECK(scs_pb_open(&cfg, pb) == SCS_OPEN_NEW_SB, "setup: first join was not NEW_SB");
    struct scs_cdt *cdt = scs_cdl_alloc(&cdl, "VMS$VAXcluster", "VMS$VAXcluster", pb);
    CHECK(cdt != NULL, "setup: no CDT");

    struct scs_depart_stats st;
    CHECK(scs_pb_depart(&cdl, &cfg, pb, &st) == SCS_PB_CLOSE_NOTHING,
          "the kill switch did not suppress the teardown");
    CHECK(st.connections_lost == 0 && st.waiters_flushed == 0 && st.mfreeq_reclaimed == 0,
          "the kill switch reported work it did not do");
    CHECK(pb->in_use == 1, "the kill switch still closed the Path Block");
    CHECK(scs_pb_cdt_count(pb) == 1, "the kill switch still released the connection");
    CHECK(scs_cdl_in_use_count(&cdl) == 1, "the kill switch still released the CDT");

    /* And with the switch off again the SAME call tears the SAME structures
     * down: the difference is the switch and nothing else. */
    unsetenv("OVMX_NO_PEER_DEPART");
    CHECK(scs_pb_depart(&cdl, &cfg, pb, &st) == SCS_PB_CLOSE_OK,
          "the bracketing control did not tear down");
    CHECK(st.connections_lost == 1, "the bracketing control lost %u connections, expected 1",
          st.connections_lost);
    CHECK(pb->in_use == 0, "the bracketing control left the Path Block in use");

    /* The listen timeout override, and its refusals. */
    setenv("OVMX_PEER_LISTEN_TIMEOUT_MS", "1500", 1);
    CHECK(scs_depart_listen_timeout_ms() == 1500, "the listen-timeout override was ignored");
    setenv("OVMX_PEER_LISTEN_TIMEOUT_MS", "0", 1);
    CHECK(scs_depart_listen_timeout_ms() == SCS_DEPART_LISTEN_TIMEOUT_DEFAULT_MS,
          "a 0ms listen timeout was accepted -- it would depart every peer at once");
    setenv("OVMX_PEER_LISTEN_TIMEOUT_MS", "banana", 1);
    CHECK(scs_depart_listen_timeout_ms() == SCS_DEPART_LISTEN_TIMEOUT_DEFAULT_MS,
          "a non-numeric listen timeout was not rejected");
    unsetenv("OVMX_PEER_LISTEN_TIMEOUT_MS");
}

/* ======================================================================
 * vms-22e: the p. 2-21 footnote anti-masquerade tests.
 *
 * "At this time, special tests are made to ensure that the remote node is not
 *  masquerading as a node already known to the local system. For example, if the
 *  SCS System ID in the formative System Block matches the SCS System ID in a
 *  System Block already in the Configuration Queue, the SCS Node Names must also
 *  match. The converse is also true. If both items match, and if there is a Path
 *  Block already queued to the System Block in the Configuration Queue, then the
 *  64-bit incarnation numbers must also match. Virtual circuit formation is
 *  abandoned if any of these tests fail."   (Davis, ch. 2, p. 2-21, footnote)
 *
 * This rule was tested against the vms-2f3 rejoin failure and REFUTED as its
 * cause (docs/HANDOFF-vms-2f3.md sec 4M.31). Nothing here fixes that bug.
 * ====================================================================== */

/* A node already in the configuration queue, with one open circuit on `pdt`. */
static struct scs_sb *seed_known_node(struct scs_config *cfg, struct scs_pdt *pdt,
                                      uint16_t scssystemid, const char *node,
                                      uint64_t incarnation, const uint8_t mac[6])
{
    struct scs_sb_info info = make_info(scssystemid, node, incarnation, 7);
    struct scs_pb *pb = scs_pb_create(cfg, pdt, mac, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(cfg, pb, &info);
    if (scs_pb_open(cfg, pb) != SCS_OPEN_NEW_SB) {
        return NULL;
    }
    return pb->sb;
}

/* Assert an abandoned formation left EVERYTHING as it was: the PB is still
 * formative on its PDT, CLOSED and abandoned, and the configuration queue is
 * untouched. Shared by the failure cases so each of them checks it.
 * `sb_count` is how many System Blocks the queue held BEFORE the abandoned
 * open -- a parameter rather than a constant 1 because the queue-depth case
 * below deliberately runs against a deeper queue. */
static void check_abandoned(struct scs_config *cfg, struct scs_pdt *pdt,
                            struct scs_pb *pb, struct scs_sb *old,
                            enum scs_masquerade_result expect, const char *what,
                            unsigned sb_count)
{
    CHECK(pb->masquerade_fail == (int)expect,
          "%s: failing test recorded as %d (%s), expected %d", what,
          pb->masquerade_fail,
          scs_masquerade_result_name((enum scs_masquerade_result)pb->masquerade_fail),
          (int)expect);
    CHECK(pb->vc_state == SCS_VC_CLOSED, "%s: abandoned circuit is %s, expected CLOSED",
          what, scs_vc_state_name(pb->vc_state));
    CHECK(pb->fsm.abandoned == 1, "%s: fsm.abandoned was not raised", what);
    CHECK(pb->on_pdt == 1, "%s: abandoned PB was dequeued from its PDT", what);
    CHECK(scs_pdt_formative_count(pdt) == 1,
          "%s: PDT formative queue holds %u, expected the abandoned PB", what,
          scs_pdt_formative_count(pdt));
    CHECK(scs_config_sb_count(cfg) == sb_count,
          "%s: configuration queue holds %u SBs, expected %u -- the abandoned"
          " formative SB was inserted", what, scs_config_sb_count(cfg), sb_count);
    CHECK(scs_sb_pb_count(old) == 1,
          "%s: old SB holds %u PBs -- the abandoned PB was queued to it", what,
          scs_sb_pb_count(old));
    CHECK(pb->sb != old, "%s: abandoned PB was re-pointed at the old SB", what);
}

/* TEST 1: System IDs match, SCS Node Names do not -> abandon. */
static void test_masquerade_node_name_mismatch_abandons(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt1, pdt2;
    const uint8_t mac_a[6] = {0x08, 0x00, 0x2b, 0x22, 0xe0, 0x01};
    const uint8_t mac_b[6] = {0x08, 0x00, 0x2b, 0x22, 0xe0, 0x02};

    scs_config_init(&cfg);
    scs_pdt_init(&pdt1, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_pdt_init(&pdt2, SCS_PORT_TYPE_ETHERNET, 1498);

    struct scs_sb *old = seed_known_node(&cfg, &pdt1, 1025, "VAX1", 0x1000ull, mac_a);
    CHECK(old != NULL, "seeding the known node failed");

    /* An impostor presenting VAX1's System ID under a different node name. */
    struct scs_sb_info fake = make_info(1025, "EVIL", 0x1000ull, 7);
    struct scs_pb *pb = scs_pb_create(&cfg, &pdt2, mac_b, SCS_PORT_TYPE_ETHERNET);
    struct scs_sb *formative = scs_pb_attach_formative_sb(&cfg, pb, &fake);
    CHECK(scs_config_masquerade_check(&cfg, formative) == SCS_MASQ_FAIL_NODE_NAME,
          "the ID-match/name-mismatch test did not fire");

    CHECK(scs_pb_open(&cfg, pb) == SCS_OPEN_ABANDONED_MASQUERADE,
          "formation was not abandoned for a node-name mismatch");
    check_abandoned(&cfg, &pdt2, pb, old, SCS_MASQ_FAIL_NODE_NAME, "name mismatch", 1);
}

/* TEST 2, THE CONVERSE: SCS Node Names match, System IDs do not -> abandon.
 * Note this lands in the p. 2-21 "learned for the first time" branch (the
 * System-ID lookup misses), which is why the tests scan the whole queue. */
static void test_masquerade_system_id_mismatch_abandons(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt1, pdt2;
    const uint8_t mac_a[6] = {0x08, 0x00, 0x2b, 0x22, 0xe1, 0x01};
    const uint8_t mac_b[6] = {0x08, 0x00, 0x2b, 0x22, 0xe1, 0x02};

    scs_config_init(&cfg);
    scs_pdt_init(&pdt1, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_pdt_init(&pdt2, SCS_PORT_TYPE_ETHERNET, 1498);

    struct scs_sb *old = seed_known_node(&cfg, &pdt1, 1025, "VAX1", 0x1000ull, mac_a);
    CHECK(old != NULL, "seeding the known node failed");
    /* Control: without the tests this would be a plain first-contact join. */
    struct scs_sb_info fake = make_info(1099, "VAX1", 0x1000ull, 7);
    CHECK(scs_config_find_sb(&cfg, fake.system_id) == NULL,
          "this scenario is not the 'learned for the first time' branch -- the"
          " converse test would be reachable from the ID-match branch and the"
          " queue scan would be untested");

    struct scs_pb *pb = scs_pb_create(&cfg, &pdt2, mac_b, SCS_PORT_TYPE_ETHERNET);
    struct scs_sb *formative = scs_pb_attach_formative_sb(&cfg, pb, &fake);
    CHECK(scs_config_masquerade_check(&cfg, formative) == SCS_MASQ_FAIL_SYSTEM_ID,
          "the converse (name-match/ID-mismatch) test did not fire");

    CHECK(scs_pb_open(&cfg, pb) == SCS_OPEN_ABANDONED_MASQUERADE,
          "formation was not abandoned for a System ID mismatch");
    check_abandoned(&cfg, &pdt2, pb, old, SCS_MASQ_FAIL_SYSTEM_ID, "ID mismatch", 1);
}

/* TEST 3: both match, a Path Block IS queued to the old SB, incarnations
 * differ -> abandon. */
static void test_masquerade_incarnation_mismatch_abandons(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt1, pdt2;
    const uint8_t mac_a[6] = {0x08, 0x00, 0x2b, 0x22, 0xe2, 0x01};
    const uint8_t mac_b[6] = {0x08, 0x00, 0x2b, 0x22, 0xe2, 0x02};

    scs_config_init(&cfg);
    scs_pdt_init(&pdt1, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_pdt_init(&pdt2, SCS_PORT_TYPE_ETHERNET, 1498);

    struct scs_sb *old = seed_known_node(&cfg, &pdt1, 1026, "VAX2", 0xAAAAAAAAull, mac_a);
    CHECK(old != NULL, "seeding the known node failed");
    CHECK(scs_sb_pb_count(old) == 1,
          "the old SB has no Path Block queued -- the incarnation test does not"
          " apply and this case would pass vacuously");

    struct scs_sb_info fake = make_info(1026, "VAX2", 0xBBBBBBBBull, 7);
    struct scs_pb *pb = scs_pb_create(&cfg, &pdt2, mac_b, SCS_PORT_TYPE_ETHERNET);
    struct scs_sb *formative = scs_pb_attach_formative_sb(&cfg, pb, &fake);
    CHECK(scs_config_masquerade_check(&cfg, formative) == SCS_MASQ_FAIL_INCARNATION,
          "the incarnation test did not fire");

    CHECK(scs_pb_open(&cfg, pb) == SCS_OPEN_ABANDONED_MASQUERADE,
          "formation was not abandoned for an incarnation mismatch");
    check_abandoned(&cfg, &pdt2, pb, old, SCS_MASQ_FAIL_INCARNATION, "incarnation", 1);
    CHECK(old->incarnation == 0xAAAAAAAAull,
          "the old SB was refreshed by a rejected formative SB (0x%llx)",
          (unsigned long long)old->incarnation);
}

/*
 * TEST 3b, THE OTHER DIRECTION: the footnote says the incarnations "must also
 * match" -- it is an EQUALITY, not an ordering. Test 3 only ever presents a
 * formative incarnation ABOVE the queued one (0xBBBBBBBB against 0xAAAAAAAA),
 * which an ordered comparison (`>` instead of `!=`) satisfies just as well. A
 * node whose incarnation goes DOWN -- a masquerader replaying an old value, or
 * a node whose clock-derived incarnation regressed -- would then be admitted
 * silently. This case presents the smaller value against the larger one, so an
 * asymmetric compare reds here.
 *
 * The header claims this rule "is correct the moment the parser supplies the
 * field". A rule tested in one direction only cannot support that claim.
 */
static void test_masquerade_incarnation_compare_is_symmetric(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt1, pdt2;
    const uint8_t mac_a[6] = {0x08, 0x00, 0x2b, 0x22, 0xe7, 0x01};
    const uint8_t mac_b[6] = {0x08, 0x00, 0x2b, 0x22, 0xe7, 0x02};

    /* (a) formative BELOW queued: 0xAAAAAAAA presented against 0xBBBBBBBB. */
    scs_config_init(&cfg);
    scs_pdt_init(&pdt1, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_pdt_init(&pdt2, SCS_PORT_TYPE_ETHERNET, 1498);
    struct scs_sb *old = seed_known_node(&cfg, &pdt1, 1026, "VAX2", 0xBBBBBBBBull, mac_a);
    CHECK(old != NULL, "seeding the known node failed");
    CHECK(scs_sb_pb_count(old) == 1,
          "the old SB has no Path Block queued -- the incarnation test does not"
          " apply and this case would pass vacuously");

    struct scs_sb_info lower = make_info(1026, "VAX2", 0xAAAAAAAAull, 7);
    struct scs_pb *pb = scs_pb_create(&cfg, &pdt2, mac_b, SCS_PORT_TYPE_ETHERNET);
    struct scs_sb *formative = scs_pb_attach_formative_sb(&cfg, pb, &lower);
    CHECK(scs_config_masquerade_check(&cfg, formative) == SCS_MASQ_FAIL_INCARNATION,
          "a DECREASING incarnation did not fire the test -- the comparison is"
          " ordered, not an equality");
    CHECK(scs_pb_open(&cfg, pb) == SCS_OPEN_ABANDONED_MASQUERADE,
          "formation was not abandoned for a decreasing incarnation");
    check_abandoned(&cfg, &pdt2, pb, old, SCS_MASQ_FAIL_INCARNATION,
                    "decreasing incarnation", 1);
    CHECK(old->incarnation == 0xBBBBBBBBull,
          "the old SB was refreshed by a rejected formative SB (0x%llx)",
          (unsigned long long)old->incarnation);

    /* (b) the boundary an ordered compare would also get wrong in the other
     * direction: EQUAL incarnations must be admitted, not abandoned. */
    scs_config_init(&cfg);
    scs_pdt_init(&pdt1, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_pdt_init(&pdt2, SCS_PORT_TYPE_ETHERNET, 1498);
    old = seed_known_node(&cfg, &pdt1, 1026, "VAX2", 0xBBBBBBBBull, mac_a);
    CHECK(old != NULL, "seeding the known node failed (b)");
    struct scs_sb_info same = make_info(1026, "VAX2", 0xBBBBBBBBull, 7);
    pb = scs_pb_create(&cfg, &pdt2, mac_b, SCS_PORT_TYPE_ETHERNET);
    formative = scs_pb_attach_formative_sb(&cfg, pb, &same);
    CHECK(scs_config_masquerade_check(&cfg, formative) == SCS_MASQ_PASS,
          "a node presenting the SAME incarnation on a second port was called a"
          " masquerader");
    CHECK(scs_pb_open(&cfg, pb) == SCS_OPEN_EXISTING_SB,
          "a second port with a matching incarnation was not admitted");
}

/*
 * TEST 3c, QUEUE DEPTH: the victim System Block is NOT the head of the
 * Configuration Queue.
 *
 * Every other case here seeds exactly one node, so the whole scan is satisfied
 * by examining cfg->sb_head and the `old = old->next` walk in
 * scs_config_masquerade_check() is never needed. Mutating that walk to stop
 * after the head leaves the rest of this file green -- i.e. the central
 * security property was untested against any queue deeper than one, which is
 * every real cluster. cfg_queue_insert() inserts at the HEAD, so seeding VAX1
 * and then VAX2 puts the victim VAX1 second, behind an unrelated node.
 */
static void test_masquerade_scan_reaches_a_non_head_sb(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt1, pdt2, pdt3;
    const uint8_t mac_a[6] = {0x08, 0x00, 0x2b, 0x22, 0xe8, 0x01};
    const uint8_t mac_b[6] = {0x08, 0x00, 0x2b, 0x22, 0xe8, 0x02};
    const uint8_t mac_c[6] = {0x08, 0x00, 0x2b, 0x22, 0xe8, 0x03};

    scs_config_init(&cfg);
    scs_pdt_init(&pdt1, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_pdt_init(&pdt2, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_pdt_init(&pdt3, SCS_PORT_TYPE_ETHERNET, 1498);

    struct scs_sb *victim = seed_known_node(&cfg, &pdt1, 1025, "VAX1", 0x1000ull, mac_a);
    CHECK(victim != NULL, "seeding the victim node failed");
    struct scs_sb *decoy = seed_known_node(&cfg, &pdt2, 1026, "VAX2", 0x2000ull, mac_b);
    CHECK(decoy != NULL, "seeding the decoy node failed");
    CHECK(scs_config_sb_count(&cfg) == 2, "two nodes were not queued");

    /* The premise, asserted rather than assumed: if the victim were the head
     * this case would degenerate into TEST 1 and prove nothing new. */
    CHECK(cfg.sb_head == decoy,
          "the decoy is not at the head of the configuration queue -- the"
          " insertion order this case depends on has changed");
    CHECK(cfg.sb_head->next == victim,
          "the victim is not behind the decoy -- this case no longer tests the"
          " queue walk");

    /* An impostor presenting the NON-HEAD node's System ID under another name.
     * Reaching it requires the scan to advance past the head. */
    struct scs_sb_info fake = make_info(1025, "EVIL", 0x1000ull, 7);
    struct scs_pb *pb = scs_pb_create(&cfg, &pdt3, mac_c, SCS_PORT_TYPE_ETHERNET);
    struct scs_sb *formative = scs_pb_attach_formative_sb(&cfg, pb, &fake);
    CHECK(scs_config_masquerade_check(&cfg, formative) == SCS_MASQ_FAIL_NODE_NAME,
          "an impostor of a NON-HEAD System Block was not detected -- the scan"
          " stopped at the head of the configuration queue");
    CHECK(scs_pb_open(&cfg, pb) == SCS_OPEN_ABANDONED_MASQUERADE,
          "the impostor of a non-head node was ADMITTED");
    check_abandoned(&cfg, &pdt3, pb, victim, SCS_MASQ_FAIL_NODE_NAME,
                    "non-head victim", 2);

    /* The same depth, but the failing test is the incarnation one -- so the
     * walk is proven for the branch that needs a Path Block queued too. */
    struct scs_sb_info wrong_inc = make_info(1025, "VAX1", 0x9999ull, 7);
    struct scs_pb *pb2 = scs_pb_create(&cfg, &pdt3, mac_c, SCS_PORT_TYPE_ETHERNET);
    struct scs_sb *formative2 = scs_pb_attach_formative_sb(&cfg, pb2, &wrong_inc);
    CHECK(scs_config_masquerade_check(&cfg, formative2) == SCS_MASQ_FAIL_INCARNATION,
          "the incarnation test did not reach a non-head System Block");
    CHECK(scs_pb_open(&cfg, pb2) == SCS_OPEN_ABANDONED_MASQUERADE,
          "a bad incarnation against a non-head node was ADMITTED");
    CHECK(victim->incarnation == 0x1000ull,
          "the non-head victim SB was refreshed by a rejected formative SB");
    CHECK(decoy->incarnation == 0x2000ull, "the decoy SB was disturbed");
}

/* TEST 4: both match, NO Path Block queued to the old SB -> the incarnation
 * test does not apply, the p. 2-21 Note refresh runs and formation proceeds.
 * This is the rebooted node, and it is why the pb_head guard is the rule. */
static void test_masquerade_no_pb_queued_refreshes(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt;
    const uint8_t mac[6] = {0x08, 0x00, 0x2b, 0x22, 0xe3, 0x01};

    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 1498);

    struct scs_sb *old = seed_known_node(&cfg, &pdt, 1027, "VAX3", 0x1000ull, mac);
    CHECK(old != NULL, "seeding the known node failed");
    scs_pb_close(&cfg, old->pb_head); /* departure: the SB stays, p. 2-17 */
    CHECK(scs_sb_pb_count(old) == 0, "departure left a Path Block queued");

    /* Rebooted: same identity pair, a NEW incarnation -- which is exactly what a
     * real rebooting node presents and must NOT be read as a masquerade. */
    struct scs_sb_info boot2 = make_info(1027, "VAX3", 0x2000ull, 7);
    struct scs_pb *pb = scs_pb_create(&cfg, &pdt, mac, SCS_PORT_TYPE_ETHERNET);
    struct scs_sb *formative = scs_pb_attach_formative_sb(&cfg, pb, &boot2);
    CHECK(scs_config_masquerade_check(&cfg, formative) == SCS_MASQ_PASS,
          "a rebooted node with no queued Path Block was called a masquerader");

    CHECK(scs_pb_open(&cfg, pb) == SCS_OPEN_EXISTING_REFRESHED,
          "the rebooted node was not admitted through the p. 2-21 Note refresh");
    CHECK(pb->vc_state == SCS_VC_OPEN, "the admitted circuit is %s, expected OPEN",
          scs_vc_state_name(pb->vc_state));
    CHECK(pb->fsm.abandoned == 0, "an admitted circuit was marked abandoned");
    CHECK(pb->masquerade_fail == (int)SCS_MASQ_PASS,
          "an admitted circuit recorded a masquerade failure (%d)", pb->masquerade_fail);
    CHECK(old->incarnation == 0x2000ull,
          "the old SB was not refreshed with the new incarnation (0x%llx)",
          (unsigned long long)old->incarnation);
    CHECK(scs_sb_pb_count(old) == 1, "the rejoined PB was not queued to the old SB");
}

/* THE KILL-SWITCH, bracketed: the same scenario abandons with the tests on and
 * is admitted with OVMX_NO_MASQUERADE_TESTS=1. Guardrail 23 -- the switch must
 * be RUN and shown to suppress the gated behaviour, not merely shipped. */
static void run_incarnation_mismatch(enum scs_open_result *result_out,
                                     enum scs_masquerade_result *check_out)
{
    struct scs_config cfg;
    struct scs_pdt pdt1, pdt2;
    const uint8_t mac_a[6] = {0x08, 0x00, 0x2b, 0x22, 0xe4, 0x01};
    const uint8_t mac_b[6] = {0x08, 0x00, 0x2b, 0x22, 0xe4, 0x02};

    scs_config_init(&cfg);
    scs_pdt_init(&pdt1, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_pdt_init(&pdt2, SCS_PORT_TYPE_ETHERNET, 1498);
    (void)seed_known_node(&cfg, &pdt1, 1026, "VAX2", 0xAAAAAAAAull, mac_a);

    struct scs_sb_info fake = make_info(1026, "VAX2", 0xBBBBBBBBull, 7);
    struct scs_pb *pb = scs_pb_create(&cfg, &pdt2, mac_b, SCS_PORT_TYPE_ETHERNET);
    struct scs_sb *formative = scs_pb_attach_formative_sb(&cfg, pb, &fake);
    *check_out = scs_config_masquerade_check(&cfg, formative);
    *result_out = scs_pb_open(&cfg, pb);
}

static void test_masquerade_kill_switch(void)
{
    enum scs_open_result result;
    enum scs_masquerade_result checked;

    unsetenv("OVMX_NO_MASQUERADE_TESTS");
    CHECK(scs_masquerade_tests_enabled() == 1, "tests are off with no switch set");
    run_incarnation_mismatch(&result, &checked);
    CHECK(result == SCS_OPEN_ABANDONED_MASQUERADE,
          "control: the gated behaviour did not happen with the switch unset (%d)",
          (int)result);

    setenv("OVMX_NO_MASQUERADE_TESTS", "1", 1);
    CHECK(scs_masquerade_tests_enabled() == 0, "the kill-switch did not disable the tests");
    run_incarnation_mismatch(&result, &checked);
    CHECK(checked == SCS_MASQ_PASS, "the check still reported a failure under the switch");
    CHECK(result == SCS_OPEN_EXISTING_SB,
          "OVMX_NO_MASQUERADE_TESTS=1 did not suppress the abandonment (%d)",
          (int)result);

    /* Only "1" disables; anything else leaves the tests on. */
    setenv("OVMX_NO_MASQUERADE_TESTS", "0", 1);
    CHECK(scs_masquerade_tests_enabled() == 1, "OVMX_NO_MASQUERADE_TESTS=0 disabled the tests");
    setenv("OVMX_NO_MASQUERADE_TESTS", "yes", 1);
    CHECK(scs_masquerade_tests_enabled() == 1, "a non-'1' value disabled the tests");

    unsetenv("OVMX_NO_MASQUERADE_TESTS");
    run_incarnation_mismatch(&result, &checked);
    CHECK(result == SCS_OPEN_ABANDONED_MASQUERADE,
          "the tests did not come back after the switch was removed (%d)", (int)result);
}

/*
 * A test whose inputs are not both populated is INDETERMINATE and must not
 * convict a node (scs_config.h: never invent). This is the shape the LIVE
 * daemon actually produces -- scs_pb_learn_system_addr fills the System Address
 * and nothing else -- so it also pins the reachability claim: OVMX cannot
 * abandon a circuit for masquerade with the System Blocks it builds today.
 */
static void test_masquerade_unknown_fields_do_not_convict(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt1, pdt2;
    const uint8_t mac_a[6] = {0x08, 0x00, 0x2b, 0x22, 0xe5, 0x01};
    const uint8_t mac_b[6] = {0x08, 0x00, 0x2b, 0x22, 0xe5, 0x02};
    uint8_t sysid[SCS_SYSTEM_ID_LEN];
    sysid_from_scssystemid(1030, sysid);

    /* (a) Daemon shape: two ports, one node, System Address only. */
    scs_config_init(&cfg);
    scs_pdt_init(&pdt1, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_pdt_init(&pdt2, SCS_PORT_TYPE_ETHERNET, 1498);
    struct scs_pb *pb1 = scs_pb_create(&cfg, &pdt1, mac_a, SCS_PORT_TYPE_ETHERNET);
    scs_pb_learn_system_addr(&cfg, pb1, sysid);
    CHECK(scs_pb_open(&cfg, pb1) == SCS_OPEN_NEW_SB, "daemon-shaped first join failed");
    CHECK(pb1->sb->node_name[0] == '\0' && pb1->sb->incarnation == 0,
          "this test no longer models what the daemon builds -- a node name or"
          " incarnation appeared, so it would stop covering the unknown-input rule");
    struct scs_pb *pb2 = scs_pb_create(&cfg, &pdt2, mac_b, SCS_PORT_TYPE_ETHERNET);
    scs_pb_learn_system_addr(&cfg, pb2, sysid);
    CHECK(scs_pb_open(&cfg, pb2) == SCS_OPEN_EXISTING_SB,
          "a System-Address-only System Block was rejected as a masquerader");

    /* (b) Formative knows its node name, the queued SB does not: the name
     * comparison has nothing to compare against and must not fire. */
    scs_config_init(&cfg);
    scs_pdt_init(&pdt1, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_pdt_init(&pdt2, SCS_PORT_TYPE_ETHERNET, 1498);
    pb1 = scs_pb_create(&cfg, &pdt1, mac_a, SCS_PORT_TYPE_ETHERNET);
    scs_pb_learn_system_addr(&cfg, pb1, sysid);
    CHECK(scs_pb_open(&cfg, pb1) == SCS_OPEN_NEW_SB, "nameless first join failed");
    struct scs_sb_info named = make_info(1030, "VAX4", 0, 7);
    pb2 = scs_pb_create(&cfg, &pdt2, mac_b, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(&cfg, pb2, &named);
    CHECK(scs_pb_open(&cfg, pb2) == SCS_OPEN_EXISTING_SB,
          "a named formative SB was rejected against a queued SB with no name");

    /* (c) A zero System ID cannot match another zero System ID into the
     * converse test: same node name, one side's ID never learned. */
    scs_config_init(&cfg);
    scs_pdt_init(&pdt1, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_pdt_init(&pdt2, SCS_PORT_TYPE_ETHERNET, 1498);
    struct scs_sb_info known = make_info(1031, "VAX5", 0x10ull, 7);
    pb1 = scs_pb_create(&cfg, &pdt1, mac_a, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(&cfg, pb1, &known);
    CHECK(scs_pb_open(&cfg, pb1) == SCS_OPEN_NEW_SB, "named first join failed");
    struct scs_sb_info no_id;
    memset(&no_id, 0, sizeof(no_id));
    no_id.node_name = "VAX5";
    no_id.incarnation = 0x10ull;
    pb2 = scs_pb_create(&cfg, &pdt2, mac_b, SCS_PORT_TYPE_ETHERNET);
    struct scs_sb *formative = scs_pb_attach_formative_sb(&cfg, pb2, &no_id);
    CHECK(scs_config_masquerade_check(&cfg, formative) == SCS_MASQ_PASS,
          "an unlearned System ID was compared as if it were a value");

    /* (d) One incarnation known, the other not, with a PB queued: indeterminate. */
    scs_config_init(&cfg);
    scs_pdt_init(&pdt1, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_pdt_init(&pdt2, SCS_PORT_TYPE_ETHERNET, 1498);
    struct scs_sb *old = seed_known_node(&cfg, &pdt1, 1032, "VAX6", 0, mac_a);
    CHECK(old != NULL && scs_sb_pb_count(old) == 1, "seeding (d) failed");
    struct scs_sb_info with_inc = make_info(1032, "VAX6", 0x77ull, 7);
    pb2 = scs_pb_create(&cfg, &pdt2, mac_b, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(&cfg, pb2, &with_inc);
    CHECK(scs_pb_open(&cfg, pb2) == SCS_OPEN_EXISTING_SB,
          "an unlearned incarnation (0) was compared as if it were a value");
}

/* A queue holding OTHER nodes must not be dragged into the comparison: the
 * tests are about the node being admitted, not about every pair in the queue. */
static void test_masquerade_ignores_unrelated_nodes(void)
{
    struct scs_config cfg;
    struct scs_pdt pdt1, pdt2, pdt3;
    const uint8_t mac_a[6] = {0x08, 0x00, 0x2b, 0x22, 0xe6, 0x01};
    const uint8_t mac_b[6] = {0x08, 0x00, 0x2b, 0x22, 0xe6, 0x02};
    const uint8_t mac_c[6] = {0x08, 0x00, 0x2b, 0x22, 0xe6, 0x03};

    scs_config_init(&cfg);
    scs_pdt_init(&pdt1, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_pdt_init(&pdt2, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_pdt_init(&pdt3, SCS_PORT_TYPE_ETHERNET, 1498);

    CHECK(seed_known_node(&cfg, &pdt1, 1025, "VAX1", 0x1000ull, mac_a) != NULL, "seed A");
    CHECK(seed_known_node(&cfg, &pdt2, 1026, "VAX2", 0x2000ull, mac_b) != NULL, "seed B");
    CHECK(scs_config_sb_count(&cfg) == 2, "two nodes were not queued");

    struct scs_sb_info fresh = make_info(1027, "VAX3", 0x3000ull, 7);
    struct scs_pb *pb = scs_pb_create(&cfg, &pdt3, mac_c, SCS_PORT_TYPE_ETHERNET);
    struct scs_sb *formative = scs_pb_attach_formative_sb(&cfg, pb, &fresh);
    CHECK(scs_config_masquerade_check(&cfg, formative) == SCS_MASQ_PASS,
          "a third, unrelated node was called a masquerader");
    CHECK(scs_pb_open(&cfg, pb) == SCS_OPEN_NEW_SB,
          "a third, unrelated node was not admitted as a first contact");
}

int main(void)
{
    test_pb_created_closed_and_formative();
    test_formation_dialogue_and_first_contact();
    test_second_circuit_no_refresh();
    test_rejoin_refreshes_old_sb();
    test_multi_sb_multi_pb_queue();
    test_learn_system_addr_at_discovery();
    test_limits_and_null_safety();
    test_config_sys_and_path_walk_every_pb_once();
    test_config_path_on_formative_pb();
    test_config_sys_does_not_invent_unknown_fields();
    test_select_vc_skips_a_circuit_that_is_not_open();
    test_pb_close_refuses_while_connections_are_queued();
    test_depart_runs_the_p228_sequence_in_order();
    test_depart_is_what_makes_the_rejoin_refresh();
    test_depart_kill_switch();
    test_masquerade_node_name_mismatch_abandons();
    test_masquerade_system_id_mismatch_abandons();
    test_masquerade_incarnation_mismatch_abandons();
    test_masquerade_incarnation_compare_is_symmetric();
    test_masquerade_scan_reaches_a_non_head_sb();
    test_masquerade_no_pb_queued_refreshes();
    test_masquerade_kill_switch();
    test_masquerade_unknown_fields_do_not_convict();
    test_masquerade_ignores_unrelated_nodes();

    printf("test_scs_config: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
