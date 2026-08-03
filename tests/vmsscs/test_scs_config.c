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
 *
 * Pure state: this test builds no frame and opens no socket.
 */
#include <stdio.h>
#include <string.h>

#include "scs_config.h"

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
    struct scs_pb *pb1 = scs_pb_create(&cfg, &pdt1, mac_a, SCS_PORT_TYPE_ETHERNET);
    scs_pb_attach_formative_sb(&cfg, pb1, &first);
    CHECK(scs_pb_open(&cfg, pb1) == SCS_OPEN_NEW_SB, "first circuit did not create the SB");
    struct scs_sb *sb = pb1->sb;

    /* Same node (same 48-bit System ID) reached over a second local port. */
    struct scs_sb_info second = make_info(1026, "VAX2", 0xBBBBBBBBull, 7);
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
    CHECK(sb->incarnation == 0xAAAAAAAAull,
          "old SB was refreshed (incarnation 0x%llx) although another PB was queued to it",
          (unsigned long long)sb->incarnation);
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
    scs_pb_close(&cfg, pb1);
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
    scs_pb_close(&cfg, pb);
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
    scs_pb_close(&cfg, pb);
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

    printf("test_scs_config: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
