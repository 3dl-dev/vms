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

int main(void)
{
    test_pb_created_closed_and_formative();
    test_formation_dialogue_and_first_contact();
    test_second_circuit_no_refresh();
    test_rejoin_refreshes_old_sb();
    test_multi_sb_multi_pb_queue();
    test_learn_system_addr_at_discovery();
    test_limits_and_null_safety();

    printf("test_scs_config: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
