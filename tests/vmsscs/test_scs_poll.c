/*
 * test_scs_poll.c - the SCS Process Poller (vms-66f), p. 2-50.
 *
 * WHAT IS AND IS NOT PROVED HERE, stated up front because this epic has sent
 * items back for comments that outran their evidence:
 *
 *   PROVED by this file: the poller's scheduling rules (PRCPOLINTERVAL cadence,
 *   one node at a time, round-robin fairness), the connect/inquire/answer/
 *   disconnect cycle driven through the REAL scs_connect() / scs_disconnect()
 *   services and the REAL connection state machine, the notify-on-Yes-only rule,
 *   the per-(SYSAP,node) disable and re-enable, and the OVMX_NO_PROCESS_POLLER
 *   kill-switch measured as "zero frames offered to the emitter".
 *
 *   NOT proved here: that a real VAX answers OVMX's inquiry. This test's
 *   directory is a stub that answers from a table. The wire shapes it builds
 *   are covered byte-for-byte in test_scs_dir.c against the golden capture; the
 *   live exchange is the lab measurement recorded on the item.
 *
 * The emitter and the inquiry hook below are STUBS -- they record what the
 * production code asked them to send instead of opening a socket. They are the
 * port-driver half, which is exactly the seam scs_svc.h defines for this.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scs_config.h"
#include "scs_dir.h"
#include "scs_poll.h"
#include "scs_svc.h"

static int failures = 0;

static void check(int cond, const char *what)
{
    if (cond) {
        printf("  OK: %s\n", what);
    } else {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

/* --- the stub port driver ------------------------------------------------- */

struct stub {
    int connect_reqs;                 /* CONNECT_REQ actions offered */
    int disconnect_reqs;              /* DISCONNECT_REQ actions offered */
    int other_actions;
    int inquiries;                    /* lookup REQUESTs offered */
    char last_inquiry[SCS_DIR_NAME_LEN + 1];
    uint8_t last_inquiry_node[6];
    int inquiry_refuse;               /* 1 => the builder declines */
    uint8_t last_connect_frame[SCS_DIR_CONNREQ_FRAME_LEN];
    int have_connect_frame;
};

/* Identity used for the frames the stub actually builds. */
static const uint8_t ovmx_mac[6]     = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 };
static const uint8_t ovmx_logical[6] = { 0xaa, 0x00, 0x04, 0x00, 0x06, 0x04 };
static const uint8_t vax1[6]         = { 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04 };
static const uint8_t vax2[6]         = { 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04 };

static int stub_emit(void *ctx, struct scs_cdt *cdt, enum scs_conn_action act,
                     const struct scs_svc_args *args, const char **what)
{
    struct stub *s = (struct stub *)ctx;
    (void)cdt;
    if (act == SCS_CONN_ACT_SEND_CONNECT_REQ) {
        /* Build the real frame the poller's connect means, so that this test
         * exercises scs_dir_build_connect_request() rather than asserting a
         * counter next to it. */
        struct scs_dir_params p;
        memset(&p, 0, sizeof(p));
        memcpy(p.dst_mac, args->target_node, 6);
        memcpy(p.src_mac, ovmx_mac, 6);
        memcpy(p.src_logical, ovmx_logical, 6);
        memcpy(p.peer_logical, args->target_node, 6);
        p.local_conid = SCS_DIR_OVMX_POLL_CONID;
        p.recv_ack = 0;
        p.send_seq = 1;
        if (scs_dir_build_connect_request(&p, s->last_connect_frame) != 0) {
            return SCS_SVC_EMIT_REFUSED;
        }
        s->have_connect_frame = 1;
        s->connect_reqs++;
        *what = "SCS$DIRECTORY CONNECT-REQUEST (poller)";
        return SCS_SVC_EMIT_SENT;
    }
    if (act == SCS_CONN_ACT_SEND_DISCONNECT_REQ) {
        /* OVMX has no DISCONNECT_REQ builder (scs_svc.h); vms-591 owns it. The
         * honest answer is NOBUILDER, and the poller must cope with it. */
        s->disconnect_reqs++;
        return SCS_SVC_EMIT_NOBUILDER;
    }
    s->other_actions++;
    return SCS_SVC_EMIT_NOBUILDER;
}

static int stub_inquire(void *ctx, struct scs_cdt *cdt, const uint8_t node[6],
                        const char *sysap)
{
    struct stub *s = (struct stub *)ctx;
    (void)cdt;
    if (s->inquiry_refuse) {
        return 0;
    }
    /* Build the real inquiry frame -- same reason as above. */
    struct scs_dir_lookup_params lp;
    memset(&lp, 0, sizeof(lp));
    memcpy(lp.dst_mac, node, 6);
    memcpy(lp.src_mac, ovmx_mac, 6);
    memcpy(lp.src_logical, ovmx_logical, 6);
    memcpy(lp.peer_logical, node, 6);
    lp.remote_conid = 0x63050008u;
    lp.local_conid = SCS_DIR_OVMX_POLL_CONID;
    lp.opcode = SCS_DIR_OPCODE;
    lp.op = SCS_DIR_OP_LOOKUP;
    memset(lp.name, ' ', sizeof(lp.name));
    size_t n = strlen(sysap);
    if (n > SCS_DIR_NAME_LEN) {
        n = SCS_DIR_NAME_LEN;
    }
    memcpy(lp.name, sysap, n);
    uint8_t frame[SCS_DIR_LOOKUP_FRAME_LEN];
    if (scs_dir_build_lookup_request(&lp, frame) != 0) {
        return 0;
    }
    s->inquiries++;
    snprintf(s->last_inquiry, sizeof(s->last_inquiry), "%s", sysap);
    memcpy(s->last_inquiry_node, node, 6);
    return 1;
}

/* --- the notified SYSAP --------------------------------------------------- */

struct found_log {
    int      count;
    char     sysap[SCS_DIR_NAME_LEN + 1];
    uint8_t  node[6];
};

static void on_found(const char *sysap, const uint8_t node[6], void *ctx)
{
    struct found_log *f = (struct found_log *)ctx;
    f->count++;
    snprintf(f->sysap, sizeof(f->sysap), "%s", sysap);
    memcpy(f->node, node, 6);
}

/* --- fixture -------------------------------------------------------------- */

struct fixture {
    struct scs_cdl      cdl;
    struct scs_pdt      pdt;
    struct scs_svc_port port;
    struct scs_config   cfg;
    struct scs_poller   poll;
    struct stub         stub;
    struct found_log    found;
};

/* Give the fixture an OPEN circuit to `node`, through the SAME production
 * configuration-queue transitions test_scs_svc.c uses (p. 2-19/2-21). p. 2-47
 * requires an open circuit before CONNECT will select anything. */
static struct scs_pb *fixture_open_vc(struct fixture *f, const uint8_t node[6])
{
    struct scs_pb *pb = scs_pb_create(&f->cfg, &f->pdt, node, SCS_PORT_TYPE_ETHERNET);
    if (pb == NULL) {
        return NULL;
    }
    (void)scs_pb_learn_system_addr(&f->cfg, pb, node);
    (void)scs_pb_open(&f->cfg, pb);
    return pb;
}

static void fixture_init(struct fixture *f)
{
    /* The poller ships OFF (see scs_poll_enabled()); every case below is about
     * what it does when it is ON, so the fixture turns it on. The kill-switch
     * case turns it back off explicitly and measures the difference. */
    setenv("OVMX_PROCESS_POLLER", "1", 1);
    unsetenv("OVMX_NO_PROCESS_POLLER");
    memset(f, 0, sizeof(*f));
    scs_config_init(&f->cfg);
    scs_pdt_init(&f->pdt, SCS_PORT_TYPE_ETHERNET, 1498);
    scs_cdl_init(&f->cdl);
    scs_svc_port_init(&f->port, &f->cdl);
    scs_poll_init(&f->poll, &f->port, &f->cfg);
    scs_poll_set_emitters(&f->poll, stub_emit, &f->stub, stub_inquire, &f->stub);
}

/* Drive one complete cycle: tick -> opened -> answer. Returns the number of
 * inquiries the cycle put out. */
static unsigned run_cycle(struct fixture *f, uint64_t t)
{
    scs_poll_tick(&f->poll, t);
    if (scs_poll_state_of(&f->poll) != SCS_POLL_CONNECTING) {
        return 0;
    }
    return scs_poll_opened(&f->poll, t);
}

/* --- tests ---------------------------------------------------------------- */

static void test_names_and_interval(void)
{
    printf("[p. 2-50 identities and the GROUNDED PRCPOLINTERVAL]\n");
    check(strcmp(SCS_DIR_SYSAP_POLLER, "SCS$DIR_LOOKUP") == 0,
          "the Process Poller's SYSAP name is SCS$DIR_LOOKUP (p. 2-50)");
    check(strcmp(SCS_DIR_SYSAP_DIRECTORY, "SCS$DIRECTORY") == 0,
          "the directory service's SYSAP name is SCS$DIRECTORY (p. 2-50)");

    unsetenv("OVMX_PRCPOLINTERVAL");
    check(scs_poll_interval_sec() == 30,
          "PRCPOLINTERVAL defaults to 30 s (GROUNDED: SYSGEN SHOW/SCS on VAX1)");
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    check(scs_poll_interval_sec() == 5, "OVMX_PRCPOLINTERVAL overrides the default");
    setenv("OVMX_PRCPOLINTERVAL", "0", 1);
    check(scs_poll_interval_sec() == SCS_POLL_PRCPOLINTERVAL_MIN,
          "an interval below the SYSGEN minimum is clamped to 1 s, not honoured");
    setenv("OVMX_PRCPOLINTERVAL", "999999", 1);
    check(scs_poll_interval_sec() == SCS_POLL_PRCPOLINTERVAL_MAX,
          "an interval above the SYSGEN maximum is clamped to 32767 s");
    unsetenv("OVMX_PRCPOLINTERVAL");

    /* THE SHIPPED DEFAULT. With neither knob set the poller is OFF -- the lab
     * bracket in scs_poll.c is what made that the default, so a test has to
     * hold it or the next reader will "fix" it back on. */
    unsetenv("OVMX_PROCESS_POLLER");
    unsetenv("OVMX_NO_PROCESS_POLLER");
    check(scs_poll_enabled() == 0,
          "the SHIPPED DEFAULT is OFF (measured: enabled, it costs the join)");
    setenv("OVMX_PROCESS_POLLER", "1", 1);
    check(scs_poll_enabled() == 1, "OVMX_PROCESS_POLLER=1 turns it on");
    setenv("OVMX_NO_PROCESS_POLLER", "1", 1);
    check(scs_poll_enabled() == 0,
          "OVMX_NO_PROCESS_POLLER=1 OVERRIDES the opt-in -- the kill-switch wins");
    unsetenv("OVMX_NO_PROCESS_POLLER");
}

/*
 * THE ITEM'S NAMED TEST: "poller finds a listening remote SYSAP, notifies,
 * connection forms, polling for that pair stops."
 */
static void test_discovery_notifies_and_then_stops(void)
{
    printf("[discover -> notify -> connect -> polling for that pair stops]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    check(fixture_open_vc(&f, vax1) != NULL, "an OPEN virtual circuit to VAX1 exists (p. 2-47)");

    check(scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found) == 1,
          "a SYSAP requests polling for VMS$VAXcluster on ALL nodes (p. 2-50)");
    check(scs_poll_add_node(&f.poll, vax1) == 1, "VAX1 is a node the poller can see");
    check(scs_poll_polling(&f.poll, "VMS$VAXcluster", vax1) == 1,
          "polling for (VMS$VAXcluster, VAX1) is ON before any connection exists");

    unsigned sent = run_cycle(&f, 10000);
    check(f.stub.connect_reqs == 1,
          "the poller CONNECTed to the remote SCS$DIRECTORY (one CONNECT_REQ frame)");
    check(f.stub.have_connect_frame == 1, "and that frame was really built");
    check(sent == 1, "one inquiry went out on the accepted connection");
    check(strcmp(f.stub.last_inquiry, "VMS$VAXcluster") == 0,
          "the inquiry asks about VMS$VAXcluster");
    check(scs_poll_state_of(&f.poll) == SCS_POLL_INQUIRING,
          "the poller is INQUIRING while the reply is outstanding");
    check(scs_poll_pending(&f.poll) == 1, "exactly one reply is outstanding");

    /* The directory answers Yes. */
    check(scs_poll_answer(&f.poll, "VMS$VAXcluster", SCS_DIR_ANSWER_YES, 10010) == 1,
          "the affirmative answer matches the outstanding inquiry");
    check(f.found.count == 1, "the interested SYSAP was NOTIFIED (p. 2-50)");
    check(strcmp(f.found.sysap, "VMS$VAXcluster") == 0 &&
          memcmp(f.found.node, vax1, 6) == 0,
          "the notification names the discovered SYSAP and the node it is on");
    check(f.stub.disconnect_reqs == 1,
          "all replies in => the poller invoked DISCONNECT (p. 2-50)");
    check(scs_poll_pending(&f.poll) == 0, "no inquiry is left outstanding");
    check(f.poll.cycles_completed == 1, "the cycle is recorded as completed");

    /* The notified SYSAP now forms its connection; p. 2-50 disables polling. */
    scs_poll_connected(&f.poll, "VMS$VAXcluster", vax1);
    check(scs_poll_polling(&f.poll, "VMS$VAXcluster", vax1) == 0,
          "polling for (VMS$VAXcluster, VAX1) STOPS once the connection exists (p. 2-50)");

    /* And it stops for real: the next due cycle emits nothing at all. */
    scs_poll_abandon(&f.poll);
    int before = f.stub.connect_reqs;
    for (uint64_t t = 20000; t <= 60000; t += 2000) {
        scs_poll_tick(&f.poll, t);
    }
    check(f.stub.connect_reqs == before,
          "with the only name disabled on the only node, no further CONNECT is made");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

static void test_continues_on_other_nodes_and_resumes_after_loss(void)
{
    printf("[disabled on one node, still polled on others; resumes if lost]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    fixture_open_vc(&f, vax1);
    fixture_open_vc(&f, vax2);
    scs_poll_request(&f.poll, "MSCP$DISK", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);
    scs_poll_add_node(&f.poll, vax2);

    scs_poll_connected(&f.poll, "MSCP$DISK", vax1);
    check(scs_poll_polling(&f.poll, "MSCP$DISK", vax1) == 0, "disabled on VAX1");
    check(scs_poll_polling(&f.poll, "MSCP$DISK", vax2) == 1,
          "STILL polled on VAX2 -- 'will continue to look ... on nodes other than NODE_X'");

    /* The next cycle must therefore be against VAX2, never VAX1. */
    unsigned sent = run_cycle(&f, 10000);
    check(sent == 1 && memcmp(f.stub.last_inquiry_node, vax2, 6) == 0,
          "the cycle that runs is the one against VAX2");

    /* p. 2-50: if the connection is lost, polling may resume on that node. */
    scs_poll_connection_lost(&f.poll, "MSCP$DISK", vax1);
    check(scs_poll_polling(&f.poll, "MSCP$DISK", vax1) == 1,
          "polling on VAX1 RESUMES after the connection is lost (p. 2-50)");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

static void test_negative_answer_notifies_nobody(void)
{
    printf("[a 'No' notifies nobody -- and neither does an unreadable result]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "MSCP$TAPE", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);

    run_cycle(&f, 10000);
    check(scs_poll_answer(&f.poll, "MSCP$TAPE", SCS_DIR_ANSWER_NO, 10010) == 1,
          "the negative answer matches the outstanding inquiry");
    check(f.found.count == 0,
          "nobody was notified about the ABSENCE of MSCP$TAPE (p. 2-50)");
    check(f.poll.answers_no == 1 && f.poll.notifications == 0,
          "the No is counted and produced no notification");

    /* An UNKNOWN result (all-zero field) is not an answer we can read; a poller
     * that treated it as a Yes would be connecting speculatively again. */
    fixture_init(&f);
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "MSCP$TAPE", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);
    run_cycle(&f, 10000);
    scs_poll_answer(&f.poll, "MSCP$TAPE", SCS_DIR_ANSWER_UNKNOWN, 10010);
    check(f.found.count == 0 && f.poll.answers_unknown == 1,
          "an UNREADABLE result notifies nobody either");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

static void test_cadence_one_node_at_a_time(void)
{
    printf("[PRCPOLINTERVAL cadence, one node at a time, round-robin]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1); /* 5 s */
    fixture_open_vc(&f, vax1);
    fixture_open_vc(&f, vax2);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);
    scs_poll_add_node(&f.poll, vax2);

    /* Both nodes are due at t=10000, but only ONE cycle may start. */
    scs_poll_tick(&f.poll, 10000);
    check(f.stub.connect_reqs == 1, "only ONE node is polled at a time (p. 2-50)");
    uint8_t first[6];
    memcpy(first, scs_poll_current_node(&f.poll), 6);
    scs_poll_tick(&f.poll, 10001);
    check(f.stub.connect_reqs == 1, "a second cycle does not start while one is in flight");

    /* Finish it, then the ~1 s spacing must still hold before the next. */
    scs_poll_opened(&f.poll, 10002);
    scs_poll_answer(&f.poll, "VMS$VAXcluster", SCS_DIR_ANSWER_NO, 10003);
    scs_poll_abandon(&f.poll); /* the disconnect dialogue is vms-591's */
    scs_poll_tick(&f.poll, 10500);
    check(f.stub.connect_reqs == 1,
          "the next node waits ~1 s -- 'one of them will have to wait approximately"
          " one second' (p. 2-50)");
    scs_poll_tick(&f.poll, 11100);
    check(f.stub.connect_reqs == 2, "after ~1 s the OTHER node is polled");
    check(memcmp(scs_poll_current_node(&f.poll), first, 6) != 0,
          "round-robin: the second cycle is against the other node, not a repeat");

    /* Neither node may be polled twice inside one PRCPOLINTERVAL. */
    scs_poll_opened(&f.poll, 11101);
    scs_poll_answer(&f.poll, "VMS$VAXcluster", SCS_DIR_ANSWER_NO, 11102);
    scs_poll_abandon(&f.poll);
    for (uint64_t t = 12200; t < 15000; t += 200) {
        scs_poll_tick(&f.poll, t);
    }
    check(f.stub.connect_reqs == 2,
          "no node is polled more than once per PRCPOLINTERVAL (p. 2-50)");
    scs_poll_tick(&f.poll, 15100);
    check(f.stub.connect_reqs == 3, "once the interval elapses, polling resumes");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

static void test_kill_switch(void)
{
    printf("[OVMX_NO_PROCESS_POLLER=1 suppresses every frame]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);

    setenv("OVMX_NO_PROCESS_POLLER", "1", 1);
    check(scs_poll_enabled() == 0, "the kill-switch reads as disabled");
    for (uint64_t t = 10000; t <= 60000; t += 1000) {
        scs_poll_tick(&f.poll, t);
    }
    check(f.stub.connect_reqs == 0 && f.stub.inquiries == 0,
          "GATED: with the switch set, ZERO CONNECT_REQs and ZERO inquiries are built");
    check(f.poll.cycles_started == 0, "and no cycle was started");
    check(scs_poll_state_of(&f.poll) == SCS_POLL_IDLE, "the poller stays IDLE");

    /* The SAME fixture with the switch cleared must emit -- otherwise the test
     * above would pass for a poller that never worked at all. */
    unsetenv("OVMX_NO_PROCESS_POLLER");
    setenv("OVMX_PROCESS_POLLER", "1", 1);
    check(scs_poll_enabled() == 1, "the kill-switch reads as enabled once cleared");
    scs_poll_tick(&f.poll, 61000);
    check(f.stub.connect_reqs == 1,
          "CONTROL: the same fixture with the switch cleared DOES connect");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

static void test_refusals_are_honest(void)
{
    printf("[no circuit / no builder: refuse, never fake]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    /* A node the poller can see but with NO open circuit (p. 2-47). */
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);
    scs_poll_tick(&f.poll, 10000);
    check(f.stub.connect_reqs == 0 && f.poll.connect_refused == 1,
          "with no OPEN virtual circuit the CONNECT is REFUSED, not faked (p. 2-47)");
    check(scs_poll_state_of(&f.poll) == SCS_POLL_IDLE,
          "and no cycle is left in flight");

    /* An inquiry the port cannot build must not be recorded as outstanding. */
    fixture_init(&f);
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);
    f.stub.inquiry_refuse = 1;
    unsigned sent = run_cycle(&f, 10000);
    check(sent == 0 && scs_poll_pending(&f.poll) == 0,
          "an unsent inquiry is NOT counted as an outstanding reply");
    check(f.stub.disconnect_reqs == 1,
          "a cycle with nothing to wait for disconnects immediately (p. 2-50)");

    /* An answer for a name we never asked about changes nothing. */
    fixture_init(&f);
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);
    run_cycle(&f, 10000);
    check(scs_poll_answer(&f.poll, "MSCP$DISK", SCS_DIR_ANSWER_YES, 10010) == 0,
          "an answer for an un-asked name is rejected");
    check(f.found.count == 0 && f.poll.answers_unsolicited == 1,
          "and notifies nobody");
    check(scs_poll_pending(&f.poll) == 1,
          "the real outstanding inquiry is still outstanding");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

static void test_cycle_timeout(void)
{
    printf("[a cycle that never completes is abandoned, not wedged]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);

    scs_poll_tick(&f.poll, 10000);
    check(scs_poll_state_of(&f.poll) == SCS_POLL_CONNECTING,
          "the cycle is waiting for the directory to accept");
    scs_poll_tick(&f.poll, 10000 + SCS_POLL_CYCLE_TIMEOUT_MS - 1);
    check(scs_poll_state_of(&f.poll) == SCS_POLL_CONNECTING, "still waiting before the timeout");
    scs_poll_tick(&f.poll, 10000 + SCS_POLL_CYCLE_TIMEOUT_MS);
    check(scs_poll_state_of(&f.poll) == SCS_POLL_IDLE && f.poll.cycles_abandoned == 1,
          "an accept that never arrives abandons the cycle rather than wedging the poller");
    check(f.poll.cycles_completed == 0,
          "an abandoned cycle is NOT counted as completed");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

static void test_descriptors_are_real_cdts(void)
{
    printf("[the poller's connection is a real CDT on the real CDL]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);

    unsigned before = scs_cdl_in_use_count(&f.cdl);
    scs_poll_tick(&f.poll, 10000);
    check(scs_cdl_in_use_count(&f.cdl) == before + 1,
          "CONNECT allocated one CDT (p. 2-56)");
    struct scs_cdt *c = scs_cdl_lookup(&f.cdl, SCS_DIR_OVMX_POLL_CONID);
    check(c != NULL, "and it carries the poller's own Con.ID, not the directory's");
    check(scs_cdl_lookup(&f.cdl, SCS_DIR_OVMX_CONID) == NULL,
          "the poller's CDT is DISTINCT from the served SCS$DIRECTORY handle (p. 2-49)");
    if (c != NULL) {
        check(scs_conn_state_of(c) == SCS_CONN_CONNECT_SENT,
              "the connection state machine is in CONNECT SENT (Figure 2-14)");
    }
    check(f.port.connects == 1, "the service port counted one CONNECT");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

int main(void)
{
    printf("test_scs_poll: the SCS Process Poller SCS$DIR_LOOKUP (vms-66f, p. 2-50)\n");
    test_names_and_interval();
    test_discovery_notifies_and_then_stops();
    test_continues_on_other_nodes_and_resumes_after_loss();
    test_negative_answer_notifies_nobody();
    test_cadence_one_node_at_a_time();
    test_kill_switch();
    test_refusals_are_honest();
    test_cycle_timeout();
    test_descriptors_are_real_cdts();
    printf("test_scs_poll: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
