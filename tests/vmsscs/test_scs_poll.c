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
    int accept_rsps;                  /* ACCEPT_RSP actions offered */
    int accept_rsp_sent;              /* 1 => the emitter HAS an ACCEPT_RSP builder */
    int inquiries;                    /* lookup REQUESTs offered */
    char last_inquiry[SCS_DIR_NAME_LEN + 1];
    uint8_t last_inquiry_node[6];
    int inquiry_refuse;               /* 1 => the builder declines */
    int refuse_other;                 /* 1 => non-connect actions come back REFUSED */
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
        /* THIS STUB DELIBERATELY HAS NO DISCONNECT_REQ BUILDER, and the reason
         * changed in round 4: it is no longer "OVMX cannot build one". OVMX CAN
         * -- scs_disc_build_request() (vms-591), which scsd.c's poller emitter
         * drives and tests/vmsscs/test_scsd_wire.c proves on the wire. What is
         * modelled here is the OTHER port driver scs_svc.h's contract admits:
         * one with no builder, which is also OVMX itself under
         * OVMX_NO_CLEAN_SHUTDOWN=1. The poller must cope with NOBUILDER without
         * claiming a frame went out, and that is what the cases below check. A
         * case that needs the dialogue to COMPLETE cannot use this answer, so
         * it drives the state machine directly -- see
         * test_a_completed_disconnect_dialogue_releases_the_descriptor_clean. */
        s->disconnect_reqs++;
        return SCS_SVC_EMIT_NOBUILDER;
    }
    if (act == SCS_CONN_ACT_SEND_ACCEPT_RSP && s->accept_rsp_sent) {
        /* An emitter that DOES have the builder. This is the ONLY way the
         * SCS_SVC_EMIT_SENT arm of poll_emit_action() can be reached: see the
         * comment on test_emit_accounting_counts_a_sent_action. Today's
         * scsd_poll_emit() has no ACCEPT_RSP builder and returns NOBUILDER --
         * the case below this one -- so the stub carries BOTH answers and each
         * case states which emitter it is standing in for. */
        s->accept_rsps++;
        *what = "SCS$DIRECTORY ACCEPT-RESPONSE (poller, stub builder)";
        return SCS_SVC_EMIT_SENT;
    }
    s->other_actions++;
    /* REFUSED and NOBUILDER land in DIFFERENT port counters (scs_svc.h). The
     * flag lets a case pick which, so the split is measured rather than
     * assumed -- see test_emit_accounting_splits_refused_from_nobuilder. */
    return s->refuse_other ? SCS_SVC_EMIT_REFUSED : SCS_SVC_EMIT_NOBUILDER;
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

/* ==========================================================================
 * ROUND 2 (vms-66f) -- THE BRANCHES A MUTATION SWEEP FOUND UNCOVERED.
 *
 * The veracity review mutated production code under the stubs above and named
 * surviving mutants. A re-run with a harder set left 18 of the 26 mutants that
 * sweep then carried alive. EVERY case below exists because a specific mutant
 * survived without it, and each one names its mutant.
 *
 * ROUND 3 added two more (M27, M28), for two arms a sweep alone did NOT find:
 * gcov showed them never executed at all, and the surviving mutants only
 * confirmed what the zero counts already said. The sweep's mutant count is a
 * moving number and is deliberately not restated here -- the script prints it.
 *
 * NO KILL COUNT IS ASSERTED HERE. Re-derive it:
 *
 *     ./tools/cluster/scs_poll_mutation_sweep.py
 *
 * That script carries every mutant named below, runs a CONTROL first (so no
 * kill can be a
 * pre-existing red), rebuilds per mutant, and prints the count. It is hand-run,
 * not a ctest, because every mutant needs a full rebuild -- the same reason
 * tools/scs_credit_measure.py is hand-run. Its cheap document-only sibling,
 * tests/vmsscs/test_scs_dir_mutants.py, IS in ctest.
 * ========================================================================== */

/* MUTANTS KILLED: name_scopes() -> `return 1;`  and  -> `return n->all_nodes;`
 * and scs_poll_polling() dropping its name_scopes() term.
 *
 * WHY THEY SURVIVED: every scs_poll_request() call site -- in this file AND in
 * production scsd.c -- passed node == NULL. p. 2-50's SPECIFIC-NODE scope
 * ("requesting its Process Poller to look for SYSAP_X on NODE_X") had no caller
 * at all, so the whole branch was free to be wrong. */
static void test_node_scoped_request(void)
{
    printf("[p. 2-50 specific-node scope: SYSAP_X on NODE_X, and nowhere else]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    fixture_open_vc(&f, vax1);
    fixture_open_vc(&f, vax2);
    /* VAX2 is added FIRST so it occupies the slot the node scan reaches first.
     * Without that the scan short-circuits on VAX1 and never has to decide
     * anything about VAX2. */
    scs_poll_add_node(&f.poll, vax2);
    scs_poll_add_node(&f.poll, vax1);

    check(scs_poll_request(&f.poll, "MSCP$DISK", vax1, on_found, &f.found) == 1,
          "a SYSAP requests polling for MSCP$DISK on VAX1 ONLY (p. 2-50)");
    check(scs_poll_polling(&f.poll, "MSCP$DISK", vax1) == 1,
          "polling is ON for the node the request names");
    check(scs_poll_polling(&f.poll, "MSCP$DISK", vax2) == 0,
          "polling is OFF on every OTHER node -- a node-scoped request is not "
          "an all-nodes request");

    unsigned sent = run_cycle(&f, 10000);
    check(sent == 1 && memcmp(f.stub.last_inquiry_node, vax1, 6) == 0,
          "the cycle that runs is against VAX1, the node in scope");
    check(f.poll.skipped_disabled == 1,
          "VAX2 -- reachable, but with no name in scope -- was SKIPPED and counted, "
          "not connected to for nothing");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

/* MUTANT KILLED: the round-robin index `(p->next_scan + k) % MAX` -> `k`.
 *
 * WHY IT SURVIVED: with TWO nodes and a 5 s interval the earlier slot is never
 * due twice running, so a scan that always restarts at slot 0 is
 * indistinguishable from round-robin. Three nodes at the SYSGEN MINIMUM
 * interval (1 s, where every node is due every cycle) separate them. */
static void test_round_robin_visits_every_node(void)
{
    printf("[round-robin: three always-due nodes are visited in turn]\n");
    static const uint8_t vax3[6] = { 0xaa, 0x00, 0x04, 0x00, 0x03, 0x04 };
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "1", 1); /* the SYSGEN minimum */
    fixture_open_vc(&f, vax1);
    fixture_open_vc(&f, vax2);
    fixture_open_vc(&f, vax3);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);
    scs_poll_add_node(&f.poll, vax2);
    scs_poll_add_node(&f.poll, vax3);

    uint8_t seen[3][6];
    uint64_t t = 10000;
    for (int i = 0; i < 3; i++) {
        scs_poll_tick(&f.poll, t);
        if (scs_poll_state_of(&f.poll) != SCS_POLL_CONNECTING) {
            check(0, "a cycle started");
            break;
        }
        memcpy(seen[i], scs_poll_current_node(&f.poll), 6);
        scs_poll_opened(&f.poll, t);
        scs_poll_answer(&f.poll, "VMS$VAXcluster", SCS_DIR_ANSWER_NO, t + 1);
        scs_poll_abandon(&f.poll); /* the disconnect dialogue is vms-591's */
        t += 1100;                 /* clears BOTH the ~1 s spacing and the interval */
    }
    check(f.stub.connect_reqs == 3, "three cycles ran");
    check(memcmp(seen[0], seen[1], 6) != 0 && memcmp(seen[1], seen[2], 6) != 0 &&
          memcmp(seen[0], seen[2], 6) != 0,
          "all THREE nodes were visited -- an always-due earlier slot does not "
          "starve the later ones");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

/* MUTANT KILLED: dropping the `now_ms < nd->last_poll_ms` term from the
 * interval test.
 *
 * WHY IT SURVIVED: no case ever moved the clock backwards. It matters because
 * now_ms is unsigned -- a backward step makes `now_ms - last_poll_ms` underflow
 * to ~2^64, every node reads as due, and the poller floods the wire at exactly
 * the moment the host is least healthy. */
static void test_clock_regression_does_not_flood(void)
{
    printf("[a clock that steps BACKWARD does not make every node due]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);

    scs_poll_tick(&f.poll, 20000);
    check(f.stub.connect_reqs == 1, "the node is polled at t=20000");
    scs_poll_abandon(&f.poll);

    scs_poll_tick(&f.poll, 9000);   /* the clock steps back 11 s */
    check(f.stub.connect_reqs == 1,
          "a BACKWARD clock step does NOT re-poll the node (no unsigned underflow)");
    scs_poll_tick(&f.poll, 26000);
    check(f.stub.connect_reqs == 2,
          "CONTROL: once the interval genuinely elapses the node IS polled again");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

/* MUTANTS KILLED: collapsing disconnects_unclosed into cycles_abandoned, and
 * dropping `p->descriptors_forced++` from poll_release_cdt().
 *
 * WHY THEY SURVIVED: no case ever let a cycle reach DISCONNECTING and then time
 * out. That is the ONLY path that separates "never got its answers" from "got
 * them all, and is stuck in the disconnect dialogue OVMX cannot complete"
 * (vms-591) -- the distinction scs_poll.c's comment claims to make. */
static void test_disconnect_timeout_is_counted_apart(void)
{
    printf("[a stuck DISCONNECT is not a discovery failure, and the forced "
           "release is counted]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);

    run_cycle(&f, 10000);
    check(scs_poll_answer(&f.poll, "VMS$VAXcluster", SCS_DIR_ANSWER_YES, 10010) == 1,
          "the cycle's one inquiry is answered");
    check(scs_poll_state_of(&f.poll) == SCS_POLL_DISCONNECTING,
          "the poller waits in DISCONNECTING -- one DISCONNECT closes nothing (p. 2-26)");
    check(f.poll.cycles_completed == 1, "the cycle IS recorded as completed");

    scs_poll_tick(&f.poll, 10010 + SCS_POLL_CYCLE_TIMEOUT_MS);
    check(scs_poll_state_of(&f.poll) == SCS_POLL_IDLE, "the wait times out");
    check(f.poll.disconnects_unclosed == 1 && f.poll.cycles_abandoned == 0,
          "it is counted as an UNCLOSED DISCONNECT, not as an abandoned cycle -- "
          "the answers did arrive");
    check(f.poll.descriptors_forced == 1,
          "and the descriptor the p. 2-26 dialogue never released was FORCED, "
          "and counted as forced");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

/* MUTANT KILLED: widening the tick() fast path from
 * `state == DISCONNECTING && cdt && close_if_closed(...)` to
 * `cdt && close_if_closed(...)`.
 *
 * WHY IT SURVIVED: no case ever had the poller's CDT reach CLOSED while the
 * poller was still INQUIRING. The peer dropping the virtual circuit mid-inquiry
 * does exactly that, and the mutant turns that failure into a silent return to
 * IDLE with no counter moved. */
static void test_vc_lost_mid_inquiry_is_a_failed_cycle(void)
{
    printf("[a circuit lost mid-inquiry is an ABANDONED cycle, not a quiet IDLE]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);

    unsigned sent = run_cycle(&f, 10000);
    check(sent == 1 && scs_poll_state_of(&f.poll) == SCS_POLL_INQUIRING,
          "one inquiry is outstanding");
    struct scs_cdt *c = scs_cdl_lookup(&f.cdl, SCS_DIR_OVMX_POLL_CONID);
    check(c != NULL, "the poller's CDT is findable");
    if (c != NULL) {
        (void)scs_conn_fsm_step(c, SCS_CONN_EV_VC_LOST); /* p. 2-28: any state -> CLOSED */
        check(scs_conn_state_of(c) == SCS_CONN_CLOSED, "the peer's circuit is gone");
    }
    scs_poll_tick(&f.poll, 10001);
    check(scs_poll_state_of(&f.poll) == SCS_POLL_INQUIRING,
          "the poller is STILL INQUIRING -- a closed descriptor is not an answer");
    scs_poll_tick(&f.poll, 10000 + SCS_POLL_CYCLE_TIMEOUT_MS);
    check(scs_poll_state_of(&f.poll) == SCS_POLL_IDLE &&
          f.poll.cycles_abandoned == 1 && f.poll.cycles_completed == 0,
          "it times out as an ABANDONED cycle: the inquiry never got its reply");
    check(f.poll.descriptors_forced == 0,
          "CONTROL for the forced-release counter: this descriptor was already "
          "CLOSED, so it was released the p. 2-26 way and NOT counted as forced");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

/* MUTANT KILLED: dropping the `t.illegal` guard from scs_poll_opened().
 *
 * WHY IT SURVIVED: every case fed the poller exactly one RCV_ACCEPT_REQ, from
 * CONNECT SENT, where it is legal. A duplicate ACCEPT_REQ arriving on an
 * already-OPEN connection is the case the guard exists for, and without it the
 * poller would inquire on a connection whose state machine just refused a
 * transition. */
static void test_illegal_accept_abandons_the_cycle(void)
{
    printf("[an ACCEPT the state machine refuses does not become an inquiry]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);

    scs_poll_tick(&f.poll, 10000);
    struct scs_cdt *c = scs_cdl_lookup(&f.cdl, SCS_DIR_OVMX_POLL_CONID);
    check(c != NULL && scs_conn_state_of(c) == SCS_CONN_CONNECT_SENT,
          "the poller's connection is in CONNECT SENT");
    if (c != NULL) {
        struct scs_conn_transition t1 = scs_conn_fsm_step(c, SCS_CONN_EV_RCV_ACCEPT_REQ);
        check(!t1.illegal && scs_conn_state_of(c) == SCS_CONN_OPEN,
              "the FIRST ACCEPT_REQ is legal and opens the connection (Figure 2-14)");
    }
    unsigned sent = scs_poll_opened(&f.poll, 10001);
    check(sent == 0 && f.stub.inquiries == 0,
          "the poller's own ACCEPT step is now ILLEGAL, so NO inquiry is built");
    check(scs_poll_state_of(&f.poll) == SCS_POLL_IDLE && f.poll.cycles_abandoned == 1,
          "the cycle is abandoned and counted, not carried on regardless");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

/* MUTANTS KILLED: dropping `port->unemitted++` when the poller has no emitter,
 * and folding the REFUSED arm of poll_emit_action() into unemitted.
 *
 * WHY THEY SURVIVED: the stub emitter was always present and never returned
 * REFUSED, so two of poll_emit_action()'s three arms were dead in test. The
 * comment above that function claims the poller accounts "exactly as scs_svc.c
 * does" -- that claim is what these two cases check. */
static void test_emit_accounting_splits_refused_from_nobuilder(void)
{
    printf("[the poller's frames land in the port's OWN emitted/unemitted/refused]\n");
    struct fixture f;

    /* (1) no emitter at all: both the CONNECT_REQ and the ACCEPT_RSP are
     *     unemitted, and BOTH are counted. */
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    scs_poll_set_emitters(&f.poll, NULL, NULL, stub_inquire, &f.stub);
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);
    scs_poll_tick(&f.poll, 10000);
    check(f.port.unemitted == 1 && f.stub.connect_reqs == 0,
          "with no emitter the CONNECT_REQ is UNEMITTED, not pretended sent");
    scs_poll_opened(&f.poll, 10001);
    check(f.port.unemitted == 2,
          "and the ACCEPT_RSP the poller owes is unemitted too -- the poller's "
          "frames are not hidden from the port's census");
    check(f.port.refused == 0, "nothing was refused");

    /* (2) an emitter that REFUSES a non-connect action: a different counter. */
    fixture_init(&f);
    f.stub.refuse_other = 1;
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);
    scs_poll_tick(&f.poll, 10000);
    unsigned before_refused = f.port.refused;
    scs_poll_opened(&f.poll, 10001);
    check(f.port.refused == before_refused + 1,
          "a REFUSED emit is counted as REFUSED, never as 'no builder'");
    check(f.stub.other_actions == 1, "and the emitter really was asked");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

/* MUTANT KILLED (review round 3): deleting `p->port->emitted++` from the
 * SCS_SVC_EMIT_SENT arm of poll_emit_action().
 *
 * WHY IT SURVIVED: gcov showed poll_emit_action() entered many times with the
 * SENT arm taken ZERO times -- line count 0, branch 0 taken 0%. (The review
 * reported 34 entries over the whole `ctest -L scs` suite. Re-measured here
 * with `-DCMAKE_C_FLAGS="--coverage -O0"` over the two binaries that link
 * vmsscs_poll -- test_scs_poll and test_scsd_wire -- the entry count reads 19
 * WITH the case below already in place, so 34 is the review's number and is
 * not restated as this file's. The ZERO is what both measurements agree on and
 * the only part this case rests on; re-derive with gcov -b on
 * scs_poll.c.gcda.) The
 * stub answered SENT only for SEND_CONNECT_REQ, and THAT action never reaches
 * poll_emit_action -- the poller's CONNECT_REQ goes out through scs_connect()
 * in scs_svc.c, which does its own accounting. Measured over scs_poll.c, the
 * ONLY action ever passed to poll_emit_action is SEND_ACCEPT_RSP, from the
 * single `poll_emit_action(p, t.action)` call site in scs_poll_opened() (the
 * CONNECT_SENT + RCV_ACCEPT_REQ row of scs_conn.c). So the arm is reachable
 * exactly when the installed emitter has an ACCEPT_RSP builder.
 *
 * WHAT IS AND IS NOT CLAIMED HERE: today's scsd_poll_emit() has NO ACCEPT_RSP
 * builder and returns NOBUILDER, so with the daemon's own emitter this arm does
 * not run -- test_emit_accounting_splits_refused_from_nobuilder is the case
 * that covers the daemon as it stands. What this case covers is the OTHER
 * legal answer of the scs_svc.h emit contract: an emitter is allowed to return
 * SENT for any action, and when it does, the poller must credit port->emitted
 * and NOT unemitted/refused. Nothing below asserts that OVMX builds an
 * ACCEPT_RSP frame, and the stub's `what` string says "stub builder" so a
 * reader of a failure message cannot mistake it for a production one. */
static void test_emit_accounting_counts_a_sent_action(void)
{
    printf("[a SENT action lands in port->emitted, not in unemitted or refused]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    f.stub.accept_rsp_sent = 1;   /* stand in for an emitter that HAS the builder */
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);
    scs_poll_tick(&f.poll, 10000);

    unsigned long emitted_before   = f.port.emitted;
    unsigned long unemitted_before = f.port.unemitted;
    unsigned long refused_before   = f.port.refused;

    scs_poll_opened(&f.poll, 10001);

    check(f.stub.accept_rsps == 1,
          "the ACCEPT_RSP the CONNECT_SENT+RCV_ACCEPT_REQ row owes really was "
          "offered to the emitter -- the arm below is not counting a no-op");
    check(f.port.emitted == emitted_before + 1,
          "a SENT emit is credited to port->emitted (the poller accounts "
          "'exactly as scs_svc.c does', which is what that comment claims)");
    check(f.port.unemitted == unemitted_before,
          "and it is NOT counted as 'no builder'");
    check(f.port.refused == refused_before, "nor as refused");
    check(f.stub.other_actions == 0,
          "the SENT answer was taken on the ACCEPT_RSP itself, not on some "
          "other action falling through the stub");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

/* MUTANT KILLED (review round 3, the adversary's X2): replacing the shift-down
 * memcpy in scs_poll_answer()'s "drop the answered inquiry" loop with `;`.
 *
 * WHY IT SURVIVED: gcov showed the loop guard at that line evaluated many times
 * (the review counted 20; the re-measurement described on the case above reads
 * 14 with this case already in place -- same caveat, the review's number is not
 * restated as ours) and its BODY executed ZERO times -- line count 0, the shift
 * branch taken 0%. Every case in this file answered either a
 * single-name cycle or the LAST entry of the pending list, and in both of those
 * `hit + 1 == pending_count`, so the loop never iterates and pending_count--
 * alone is enough. Answering a MIDDLE entry is the only shape that compacts,
 * and with the memcpy gone the tail entry is silently DUPLICATED over the
 * answered slot and the last real name is dropped -- the poller would then
 * re-notify on a name it already answered and never wait for the one it lost. */
static void test_answering_a_middle_inquiry_compacts_the_pending_list(void)
{
    printf("[answering a MIDDLE inquiry compacts the pending list in order]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    fixture_open_vc(&f, vax1);
    /* Three names, so there IS a middle. Registration order is the order
     * scs_poll_opened() puts them into `pending`. */
    scs_poll_request(&f.poll, "MSCP$DISK", NULL, on_found, &f.found);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_request(&f.poll, "MSCP$TAPE", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);

    unsigned sent = run_cycle(&f, 10000);
    check(sent == 3, "the cycle asks about all three names");
    check(scs_poll_pending(&f.poll) == 3, "and all three are pending");
    check(strcmp(f.poll.pending[0], "MSCP$DISK") == 0 &&
          strcmp(f.poll.pending[1], "VMS$VAXcluster") == 0 &&
          strcmp(f.poll.pending[2], "MSCP$TAPE") == 0,
          "the pending list is in registration order before the answer");

    /* Answer the MIDDLE one. hit == 1, pending_count == 3, so the loop body
     * runs exactly once: pending[1] <- pending[2]. */
    check(scs_poll_answer(&f.poll, "VMS$VAXcluster", SCS_DIR_ANSWER_NO, 10002) == 1,
          "the middle inquiry is recognised and consumed");
    check(scs_poll_pending(&f.poll) == 2, "two inquiries are still outstanding");
    check(strcmp(f.poll.pending[0], "MSCP$DISK") == 0,
          "the entry BEFORE the answered one is untouched");
    check(strcmp(f.poll.pending[1], "MSCP$TAPE") == 0,
          "and the entry AFTER it shifted down into the hole -- without the "
          "shift, pending[1] would still read VMS$VAXcluster and MSCP$TAPE "
          "would be unreachable while pending_count says 2");
    check(scs_poll_state_of(&f.poll) == SCS_POLL_INQUIRING,
          "the cycle is still running -- the compaction happened mid-cycle");

    /* The survivors must still be ANSWERABLE, which is the consequence a
     * duplicated slot destroys: with the memcpy gone, MSCP$TAPE is not in the
     * list at all and this answer would be scored unsolicited. */
    unsigned unsolicited_before = f.poll.answers_unsolicited;
    check(scs_poll_answer(&f.poll, "MSCP$TAPE", SCS_DIR_ANSWER_NO, 10003) == 1,
          "the shifted entry is still findable by name");
    check(f.poll.answers_unsolicited == unsolicited_before,
          "and it was NOT scored as an unsolicited answer");
    check(scs_poll_answer(&f.poll, "MSCP$DISK", SCS_DIR_ANSWER_NO, 10004) == 1,
          "so is the one that never moved");
    check(scs_poll_pending(&f.poll) == 0, "the cycle drained");
    check(f.poll.cycles_completed == 1, "and completed");

    /* A second answer for the name already consumed is unsolicited -- it was
     * removed, not merely skipped. */
    check(scs_poll_answer(&f.poll, "VMS$VAXcluster", SCS_DIR_ANSWER_NO, 10005) == 0,
          "the answered middle name is GONE from the list, not still in it");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

/* MUTANT KILLED: deleting the disabled-name skip (and its skipped_disabled++)
 * from the inquiry loop in scs_poll_opened().
 *
 * WHY IT SURVIVED: every case registered ONE name, so a node was either fully
 * enabled or had nothing to ask. p. 2-50 disables a (SYSAP, node) PAIR, so the
 * case that matters is a node with one disabled name and one live one. */
static void test_disabled_name_is_not_inquired_about(void)
{
    printf("[a name already connected on this node is skipped, not asked again]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "MSCP$DISK", NULL, on_found, &f.found);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);
    scs_poll_connected(&f.poll, "MSCP$DISK", vax1);

    unsigned sent = run_cycle(&f, 10000);
    check(sent == 1, "the cycle asks about ONE of the two names");
    check(strcmp(f.stub.last_inquiry, "VMS$VAXcluster") == 0,
          "and it is the name that is NOT already connected on VAX1");
    check(f.poll.skipped_disabled == 1,
          "the disabled name was skipped and counted (p. 2-50)");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

/* MUTANT KILLED: replacing the bounded `if (n->disabled_count <
 * SCS_POLL_MAX_NODES)` deposit with a wrapping one.
 *
 * WHY IT SURVIVED: no case ever disabled a name on more than two nodes. The
 * wrapping version silently EVICTS the first node's entry, which would resume
 * polling a pair that is already connected -- exactly what p. 2-50 forbids. */
static void test_disabled_table_is_bounded_and_refuses_overflow(void)
{
    printf("[the per-name disabled table is bounded: it refuses, it does not wrap]\n");
    struct fixture f;
    fixture_init(&f);
    scs_poll_request(&f.poll, "MSCP$DISK", NULL, on_found, &f.found);

    uint8_t node[SCS_POLL_MAX_NODES + 1][6];
    for (unsigned i = 0; i <= SCS_POLL_MAX_NODES; i++) {
        memcpy(node[i], vax1, 6);
        node[i][5] = (uint8_t)(0x10 + i);
        scs_poll_connected(&f.poll, "MSCP$DISK", node[i]);
    }
    check(scs_poll_polling(&f.poll, "MSCP$DISK", node[0]) == 0,
          "the FIRST node disabled is still disabled -- it was not evicted");
    check(scs_poll_polling(&f.poll, "MSCP$DISK", node[SCS_POLL_MAX_NODES - 1]) == 0,
          "so is the last one that fit");
    check(scs_poll_polling(&f.poll, "MSCP$DISK", node[SCS_POLL_MAX_NODES]) == 1,
          "the one PAST the bound was refused -- it is still polled, which is "
          "wasteful but honest, where eviction would have been silently wrong");
}

/* MUTANT KILLED: dropping `n->disabled_count = 0;` from scs_poll_request().
 *
 * WHY IT SURVIVED: no case re-requested a name after it had been disabled.
 * p. 2-50 names that path explicitly ("once again requesting its Process Poller
 * to look for SYSAP_X on NODE_X"). */
static void test_a_fresh_request_re_enables_polling(void)
{
    printf("[re-requesting a disabled name turns polling back on (p. 2-50)]\n");
    struct fixture f;
    fixture_init(&f);
    scs_poll_request(&f.poll, "MSCP$DISK", NULL, on_found, &f.found);
    scs_poll_connected(&f.poll, "MSCP$DISK", vax1);
    check(scs_poll_polling(&f.poll, "MSCP$DISK", vax1) == 0,
          "polling is off while the connection exists");
    check(scs_poll_request(&f.poll, "MSCP$DISK", NULL, on_found, &f.found) == 1,
          "the SYSAP requests polling for it again");
    check(scs_poll_polling(&f.poll, "MSCP$DISK", vax1) == 1,
          "a FRESH REQUEST re-enables polling on every node it had been "
          "disabled on");
}

/* MUTANT KILLED: dropping the `name_trim_len(sysap) == 0` guard from
 * scs_poll_request(). A blank name would be registered, would occupy one of the
 * eight slots, and would be inquired about on every cycle -- a 16-space SYSAP
 * name on the reference wire. */
static void test_a_blank_sysap_name_is_refused(void)
{
    printf("[a blank SYSAP name is refused, not registered]\n");
    struct fixture f;
    fixture_init(&f);
    check(scs_poll_request(&f.poll, "", NULL, on_found, &f.found) == 0,
          "an empty SYSAP name is refused");
    check(scs_poll_request(&f.poll, "     ", NULL, on_found, &f.found) == 0,
          "an all-blank SYSAP name is refused too");
    check(scs_poll_polling(&f.poll, "", vax1) == 0, "and nothing was registered");
    check(scs_poll_request(&f.poll, "MSCP$DISK", NULL, on_found, &f.found) == 1,
          "CONTROL: a real name is still accepted");
}

/* MUTANT KILLED: dropping the in-flight abandon from scs_poll_drop_node().
 *
 * WHY IT SURVIVED: no case dropped a node while a cycle against it was open.
 * The circuit that carried the cycle is exactly what has gone away, so the
 * cycle cannot continue and its descriptor must come back. */
static void test_dropping_the_node_under_a_cycle_abandons_it(void)
{
    printf("[dropping the node a cycle is running against abandons the cycle]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);

    unsigned before = scs_cdl_in_use_count(&f.cdl);
    scs_poll_tick(&f.poll, 10000);
    check(scs_poll_state_of(&f.poll) == SCS_POLL_CONNECTING &&
          scs_cdl_in_use_count(&f.cdl) == before + 1,
          "a cycle is in flight against VAX1 and holds a CDT");
    scs_poll_drop_node(&f.poll, vax1);
    check(scs_poll_state_of(&f.poll) == SCS_POLL_IDLE,
          "the cycle is ABANDONED -- the circuit that carried it is gone");
    check(scs_cdl_in_use_count(&f.cdl) == before,
          "and its descriptor was given back, not leaked");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

/* MUTANT KILLED: dropping `p->answers_unsolicited++` from the not-INQUIRING
 * arm of scs_poll_answer(). The existing unsolicited case answered a name that
 * was not outstanding WHILE inquiring, which is the OTHER arm. */
static void test_an_answer_while_idle_is_unsolicited(void)
{
    printf("[an answer arriving with no cycle open is counted, not ignored]\n");
    struct fixture f;
    fixture_init(&f);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    check(scs_poll_state_of(&f.poll) == SCS_POLL_IDLE, "no cycle is open");
    check(scs_poll_answer(&f.poll, "VMS$VAXcluster", SCS_DIR_ANSWER_YES, 1) == 0,
          "the answer is rejected");
    check(f.poll.answers_unsolicited == 1,
          "and COUNTED as unsolicited -- a directory answering a question nobody "
          "asked is a fact about the peer, not noise");
    check(f.found.count == 0, "it notifies nobody");
}

/* ==========================================================================
 * ROUND 4 -- THE THREE CYCLE ENDINGS gcov SAID WERE NEVER TAKEN.
 *
 * Measured over the whole `ctest -L scs` suite before this round, scs_poll.c:
 * the tick() clean-release arm (then lines 400-403), and BOTH early returns in
 * poll_close() (then 509-511 and 515-517), executed 0 times each.
 *
 * They were dead for ONE reason, and it was not a test gap: scsd.c's poller
 * emitter answered NOBUILDER for SEND_DISCONNECT_REQ on the false ground that
 * OVMX has no such builder, so no teardown frame ever left the daemon, no peer
 * ever answered one, and no cycle could reach CLOSED. Fixing that emitter is
 * what makes the first case below a real production path rather than a
 * contrived one; the wire test proves the daemon end-to-end, and these three
 * pin the poller's own arms.
 * ========================================================================== */

/* THE CLEAN ENDING. p. 2-26's dialogue completes, the descriptor comes back
 * through scs_svc_close_if_closed(), and descriptors_forced is NOT touched.
 *
 * WHY THE EVENTS ARE FED TO scs_conn_fsm_step() DIRECTLY. This case needs a
 * connection that reaches CLOSED and is STILL HELD -- the state the tick() arm
 * exists to clean up. scs_svc_deliver() releases as it closes (that path is the
 * daemon's, and scs_poll_cdt_released() below covers it), so the caller
 * modelled here is the OTHER production one: a step of the machine that
 * releases nothing. scsd.c has exactly that call -- conn_step() on a
 * connection-control frame whose peer slot could not be resolved -- and so does
 * scs_vc_break()'s p. 2-28 sweep. Same production function this file already
 * uses in test_vc_lost_mid_inquiry_is_a_failed_cycle. */
static void test_a_completed_disconnect_dialogue_releases_the_descriptor_clean(void)
{
    printf("[the p. 2-26 dialogue completes: the descriptor comes back CLEAN]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);

    unsigned before = scs_cdl_in_use_count(&f.cdl);
    check(run_cycle(&f, 10000) == 1, "one inquiry went out");
    struct scs_cdt *c = scs_cdl_lookup(&f.cdl, SCS_DIR_OVMX_POLL_CONID);
    check(c != NULL, "the poller's CDT is findable");
    check(scs_poll_answer(&f.poll, "VMS$VAXcluster", SCS_DIR_ANSWER_YES, 10001) == 1,
          "the last inquiry is answered, so the cycle invokes DISCONNECT");
    check(scs_poll_state_of(&f.poll) == SCS_POLL_DISCONNECTING,
          "the poller waits in DISCONNECTING -- one DISCONNECT closes nothing");
    if (c == NULL) {
        unsetenv("OVMX_PRCPOLINTERVAL");
        return;
    }
    check(scs_conn_state_of(c) == SCS_CONN_DISC_SENT,
          "the connection is in DISC SENT (Figure 2-16)");

    (void)scs_conn_fsm_step(c, SCS_CONN_EV_RCV_DISCONNECT_RSP);
    check(scs_conn_state_of(c) == SCS_CONN_DISC_ACK,
          "the peer's DISCONNECT_RSP takes it to DISC ACK (p. 2-26)");
    scs_poll_tick(&f.poll, 10002);
    check(scs_poll_state_of(&f.poll) == SCS_POLL_DISCONNECTING,
          "a HALF-finished dialogue does not end the cycle");

    (void)scs_conn_fsm_step(c, SCS_CONN_EV_RCV_DISCONNECT_REQ);
    check(scs_conn_state_of(c) == SCS_CONN_CLOSED,
          "the peer's matching DISCONNECT_REQ takes it to CLOSED");
    check(scs_cdl_in_use_count(&f.cdl) == before + 1,
          "and NOTHING has released the descriptor yet -- which is the state "
          "this arm exists to clean up");

    scs_poll_tick(&f.poll, 10003);
    check(scs_poll_state_of(&f.poll) == SCS_POLL_IDLE,
          "the cycle ends on the retry, without waiting for the timeout");
    check(f.poll.disconnects_closed == 1,
          "the completed teardown is COUNTED (disconnects_closed)");
    check(f.poll.disconnects_unclosed == 0,
          "a teardown that completed is not reported as unclosed");
    check(f.poll.descriptors_forced == 0,
          "and it was NOT a forced release -- this is the p. 2-26 path");
    check(scs_cdl_in_use_count(&f.cdl) == before,
          "the descriptor is back on the CDL");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

/* THE RECEIVE PATH GOT THERE FIRST. scs_svc_deliver() closes AND releases, so
 * by the time the poller could look, `in_use` is 0 and its own retry answers
 * "not closed". Without scs_poll_cdt_released() the cycle would sit in
 * DISCONNECTING until its timeout and be counted in disconnects_unclosed -- a
 * teardown that COMPLETED reported as one that did not. */
static void test_a_release_from_the_receive_path_ends_the_cycle(void)
{
    printf("[a descriptor released by the receive path ends the cycle CLEAN]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);

    check(run_cycle(&f, 10000) == 1, "one inquiry went out");
    struct scs_cdt *c = scs_cdl_lookup(&f.cdl, SCS_DIR_OVMX_POLL_CONID);
    check(scs_poll_answer(&f.poll, "VMS$VAXcluster", SCS_DIR_ANSWER_YES, 10001) == 1,
          "the cycle invokes DISCONNECT");
    check(scs_poll_state_of(&f.poll) == SCS_POLL_DISCONNECTING, "and waits");
    if (c == NULL) {
        check(0, "the poller's CDT is findable");
        unsetenv("OVMX_PRCPOLINTERVAL");
        return;
    }
    /* The daemon's own two steps, through the REAL delivery service. */
    int closed = 0;
    (void)scs_svc_deliver(&f.port, c, SCS_CONN_EV_RCV_DISCONNECT_RSP, NULL, &closed);
    check(closed == 0, "the DISCONNECT_RSP alone does not close it");
    (void)scs_svc_deliver(&f.port, c, SCS_CONN_EV_RCV_DISCONNECT_REQ, NULL, &closed);
    check(closed == 1, "the matching DISCONNECT_REQ closes it AND releases the CDT");

    check(scs_poll_state_of(&f.poll) == SCS_POLL_DISCONNECTING,
          "CONTROL: the poller cannot see that by itself -- it is still waiting");
    scs_poll_tick(&f.poll, 10002);
    check(scs_poll_state_of(&f.poll) == SCS_POLL_DISCONNECTING,
          "CONTROL: and its own retry cannot find it either (in_use == 0)");

    check(scs_poll_cdt_released(&f.poll, c) == 1,
          "the pushed release is recognised as THIS poller's descriptor");
    check(scs_poll_state_of(&f.poll) == SCS_POLL_IDLE, "and ends the cycle");
    check(f.poll.disconnects_closed == 1, "counted as a CLOSED teardown");
    check(f.poll.disconnects_unclosed == 0 && f.poll.descriptors_forced == 0,
          "not as an unclosed one, and not as a forced release");
    check(scs_poll_cdt_released(&f.poll, c) == 0,
          "a second push for the same descriptor changes nothing");
    check(f.poll.disconnects_closed == 1, "and does not double-count");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

/* The other arm of the same function: a descriptor released while the cycle was
 * still INQUIRING. p. 2-26 lets EITHER SYSAP disconnect first, so a directory
 * that tears the connection down mid-cycle is legal -- and the inquiries still
 * outstanding never got their replies, which is an ABANDONED cycle however
 * politely the connection came down. */
static void test_a_release_mid_inquiry_is_an_abandoned_cycle(void)
{
    printf("[the DIRECTORY disconnects first: the cycle is abandoned, not closed]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);

    check(run_cycle(&f, 10000) == 1, "one inquiry is outstanding");
    struct scs_cdt *c = scs_cdl_lookup(&f.cdl, SCS_DIR_OVMX_POLL_CONID);
    if (c == NULL) {
        check(0, "the poller's CDT is findable");
        unsetenv("OVMX_PRCPOLINTERVAL");
        return;
    }
    check(scs_poll_cdt_released(&f.poll, c) == 1, "the release is ours");
    check(scs_poll_state_of(&f.poll) == SCS_POLL_IDLE, "the cycle ends");
    check(f.poll.cycles_abandoned == 1,
          "and is counted as ABANDONED -- the inquiry never got its reply");
    check(f.poll.disconnects_closed == 0,
          "NOT as a completed teardown: the poller never invoked DISCONNECT");
    check(f.poll.cycles_completed == 0, "and not as a completed cycle");
    check(scs_poll_pending(&f.poll) == 0, "the outstanding inquiry is dropped");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

/* poll_close()'s FIRST early return. In descriptorless mode (scs_svc.h) the
 * port has no CDL, CONNECT succeeds without allocating anything, and a cycle
 * ends with no descriptor to wait for and none to give back. Nothing may be
 * counted: no teardown went out and nothing was forced. */
static void test_a_descriptorless_cycle_closes_without_counting_a_teardown(void)
{
    printf("[descriptorless mode: a cycle ends with nothing to release]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    scs_svc_port_init(&f.port, NULL);   /* NO CDL -- scs_svc.h descriptorless */
    check(scs_svc_descriptors_available(&f.port) == 0, "the port has no descriptors");
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);

    check(run_cycle(&f, 10000) == 1, "the cycle still inquires");
    check(f.poll.cdt == NULL, "and holds no descriptor");
    check(scs_poll_answer(&f.poll, "VMS$VAXcluster", SCS_DIR_ANSWER_YES, 10001) == 1,
          "the last answer closes the cycle");
    check(scs_poll_state_of(&f.poll) == SCS_POLL_IDLE,
          "which goes straight to IDLE -- there is nothing to wait for");
    check(f.poll.cycles_completed == 1, "the cycle completed");
    check(f.poll.disconnects_closed == 0 && f.poll.disconnects_unclosed == 0,
          "but NO teardown is claimed either way -- none was invoked");
    check(f.poll.descriptors_forced == 0, "and nothing was force-released");
    check(f.stub.disconnect_reqs == 0,
          "the emitter was never offered a DISCONNECT_REQ: a frame with no "
          "Con.ID pair names no connection");
    unsetenv("OVMX_PRCPOLINTERVAL");
}

/* poll_close()'s SECOND early return: the descriptor was ALREADY CLOSED before
 * the cycle asked to disconnect. p. 2-28's VC_LOST sweep does exactly that --
 * it drives every connection on a broken circuit to CLOSED and releases
 * nothing. scs_disconnect() then refuses (no table row from CLOSED) and the
 * descriptor is given back the p. 2-26 way. NO teardown may be counted: none
 * went out.
 *
 * This is the sibling of test_vc_lost_mid_inquiry_is_a_failed_cycle, which
 * loses the circuit and then lets the cycle TIME OUT. This one ANSWERS the
 * outstanding inquiry instead, which is the path through poll_close(). */
static void test_a_cycle_closing_on_an_already_closed_descriptor(void)
{
    printf("[the circuit died mid-inquiry: close gives the descriptor back]\n");
    struct fixture f;
    fixture_init(&f);
    setenv("OVMX_PRCPOLINTERVAL", "5", 1);
    fixture_open_vc(&f, vax1);
    scs_poll_request(&f.poll, "VMS$VAXcluster", NULL, on_found, &f.found);
    scs_poll_add_node(&f.poll, vax1);

    unsigned before = scs_cdl_in_use_count(&f.cdl);
    check(run_cycle(&f, 10000) == 1, "one inquiry is outstanding");
    struct scs_cdt *c = scs_cdl_lookup(&f.cdl, SCS_DIR_OVMX_POLL_CONID);
    if (c == NULL) {
        check(0, "the poller's CDT is findable");
        unsetenv("OVMX_PRCPOLINTERVAL");
        return;
    }
    (void)scs_conn_fsm_step(c, SCS_CONN_EV_VC_LOST); /* p. 2-28: any state -> CLOSED */
    check(scs_conn_state_of(c) == SCS_CONN_CLOSED, "the circuit is gone");
    unsigned long illegal_before = f.port.illegal;
    int discs_before = f.stub.disconnect_reqs;

    check(scs_poll_answer(&f.poll, "VMS$VAXcluster", SCS_DIR_ANSWER_YES, 10001) == 1,
          "the last inquiry is answered, so the cycle closes");
    check(scs_poll_state_of(&f.poll) == SCS_POLL_IDLE,
          "and goes straight to IDLE -- the connection is already CLOSED");
    check(f.port.illegal == illegal_before + 1,
          "scs_disconnect() REFUSED the already-closed connection, and the "
          "refusal is counted rather than swallowed");
    check(f.stub.disconnect_reqs == discs_before,
          "so no DISCONNECT_REQ was offered to the emitter");
    check(f.poll.disconnects_closed == 0,
          "and NO teardown is claimed -- none went out");
    check(f.poll.descriptors_forced == 0,
          "the descriptor came back the p. 2-26 way, not forced");
    check(scs_cdl_in_use_count(&f.cdl) == before,
          "and it is back on the CDL");
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
    /* round 2 -- the branches the mutation sweep found uncovered */
    test_node_scoped_request();
    test_round_robin_visits_every_node();
    test_clock_regression_does_not_flood();
    test_disconnect_timeout_is_counted_apart();
    test_vc_lost_mid_inquiry_is_a_failed_cycle();
    test_illegal_accept_abandons_the_cycle();
    test_emit_accounting_splits_refused_from_nobuilder();
    test_emit_accounting_counts_a_sent_action();
    test_answering_a_middle_inquiry_compacts_the_pending_list();
    test_disabled_name_is_not_inquired_about();
    test_disabled_table_is_bounded_and_refuses_overflow();
    test_a_fresh_request_re_enables_polling();
    test_a_blank_sysap_name_is_refused();
    test_dropping_the_node_under_a_cycle_abandons_it();
    test_an_answer_while_idle_is_unsolicited();
    /* round 4 -- the three cycle endings gcov said were never taken */
    test_a_completed_disconnect_dialogue_releases_the_descriptor_clean();
    test_a_release_from_the_receive_path_ends_the_cycle();
    test_a_release_mid_inquiry_is_an_abandoned_cycle();
    test_a_descriptorless_cycle_closes_without_counting_a_teardown();
    test_a_cycle_closing_on_an_already_closed_descriptor();
    printf("test_scs_poll: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
