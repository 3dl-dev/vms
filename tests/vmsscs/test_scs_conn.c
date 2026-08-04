/*
 * test_scs_conn.c - vms-dd5: the SCA connection state machine.
 *
 * WHAT IS PROVED HERE, and what is not.
 *
 * PROVED. The three figures of VAXcluster Principles ch. 2 are transcribed ONCE
 * MORE in this file, independently of src/vmsscs/scs_conn.c, as a flat list of
 * (state, event) -> (state, action, notify) tuples read off the diagrams; the
 * test then asserts the production table agrees with every one of them AND that
 * each is flagged `documented`. That is deliberately a duplicate transcription
 * rather than a re-read of the module's own table: a test that walks the
 * implementation's table and checks it against itself proves nothing. Each of
 * the three paths (formation, rejection, disconnect) is then walked a second
 * time as a live sequence over a real CDT in a real CDL, from BOTH ends, with
 * the exact state asserted after every message in each direction.
 *
 * ALSO PROVED. The kill switch is RUN, not described (guardrail 23): the same
 * step is executed with OVMX_NO_CONN_FSM unset and set, and the test asserts
 * that with it set the state does NOT move and the log stream stays EMPTY --
 * i.e. that the switch gates the thing this module actually does.
 *
 * NOT PROVED HERE. Nothing in this file touches a socket, a frame, or a lab
 * VAX. Whether the daemon feeds the machine the RIGHT event for a given
 * received frame is a separate and weaker claim; the daemon-side wiring is
 * exercised in tests/vmsscs/test_scsd_wire.c over the real scsd.c translation
 * unit, and the frame->message mapping itself is an INFERENCE recorded in
 * docs/cluster-protocol-spec.md sec 5. This file does not make it true.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scs_conn.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            failures++;                                     \
            printf("FAIL %s:%d: ", __func__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

#define CHECK_STATE(cdt, want)                                                  \
    CHECK(scs_conn_state_of(cdt) == (want), "state is %s, expected %s",         \
          scs_conn_state_name(scs_conn_state_of(cdt)), scs_conn_state_name(want))

/* --------------------------------------------------------------------------
 * A CDL + one CDT, the way the daemon uses them. Static because struct scs_cdl
 * carries SCSCONNCNT+200 CDTs.
 * -------------------------------------------------------------------------- */
static struct scs_cdl cdl;

static struct scs_cdt *fresh_conn(const char *local, const char *remote)
{
    scs_cdl_init(&cdl);
    struct scs_cdt *c = scs_cdl_alloc(&cdl, local, remote, NULL);
    if (c != NULL) {
        scs_conn_fsm_init(c);
    }
    return c;
}

/* ==========================================================================
 * 1. THE FIGURES, transcribed independently of the production table.
 *
 * Read off Figures 2-14, 2-15 and 2-16 and the pp. 2-23/2-26/2-27 narrative.
 * Every row here must exist in the production table with exactly these
 * outputs, and must be flagged documented.
 * ========================================================================== */
struct figure_row {
    const char          *fig;
    enum scs_conn_state  from;
    enum scs_conn_event  ev;
    enum scs_conn_state  to;
    enum scs_conn_action action;
    unsigned             notify;
};

static const struct figure_row figure_rows[] = {
    /* --- Figure 2-14, NODE_1 (source) --- */
    {"2-14 N1", SCS_CONN_CLOSED, SCS_CONN_EV_SVC_CONNECT,
     SCS_CONN_CONNECT_SENT, SCS_CONN_ACT_SEND_CONNECT_REQ, 0},
    {"2-14 N1", SCS_CONN_CONNECT_SENT, SCS_CONN_EV_RCV_CONNECT_RSP,
     SCS_CONN_CONNECT_ACK, SCS_CONN_ACT_NONE, 0},
    {"2-14 N1", SCS_CONN_CONNECT_ACK, SCS_CONN_EV_RCV_ACCEPT_REQ,
     SCS_CONN_OPEN, SCS_CONN_ACT_SEND_ACCEPT_RSP, SCS_CONN_NOTIFY_ACCEPTED},
    /* --- Figure 2-14, NODE_2 (target) --- */
    {"2-14 N2", SCS_CONN_CLOSED, SCS_CONN_EV_RCV_CONNECT_REQ,
     SCS_CONN_CONNECT_REC, SCS_CONN_ACT_SEND_CONNECT_RSP,
     SCS_CONN_NOTIFY_CONNECT_REQ | SCS_CONN_NOTIFY_LISTEN_STOP},
    {"2-14 N2", SCS_CONN_CONNECT_REC, SCS_CONN_EV_SVC_ACCEPT,
     SCS_CONN_ACCEPT_SENT, SCS_CONN_ACT_SEND_ACCEPT_REQ, 0},
    {"2-14 N2", SCS_CONN_ACCEPT_SENT, SCS_CONN_EV_RCV_ACCEPT_RSP,
     SCS_CONN_OPEN, SCS_CONN_ACT_NONE, SCS_CONN_NOTIFY_LISTEN_RESUME},
    /* --- Figure 2-15 --- */
    {"2-15 N2", SCS_CONN_CONNECT_REC, SCS_CONN_EV_SVC_REJECT,
     SCS_CONN_REJECT_SENT, SCS_CONN_ACT_SEND_REJECT_REQ, 0},
    {"2-15 N1", SCS_CONN_CONNECT_ACK, SCS_CONN_EV_RCV_REJECT_REQ,
     SCS_CONN_CLOSED, SCS_CONN_ACT_SEND_REJECT_RSP, SCS_CONN_NOTIFY_REJECTED},
    {"2-15 N2", SCS_CONN_REJECT_SENT, SCS_CONN_EV_RCV_REJECT_RSP,
     SCS_CONN_CLOSED, SCS_CONN_ACT_NONE, SCS_CONN_NOTIFY_LISTEN_RESUME},
    /* --- Figure 2-16, NODE_1 --- */
    {"2-16 N1", SCS_CONN_OPEN, SCS_CONN_EV_SVC_DISCONNECT,
     SCS_CONN_DISC_SENT, SCS_CONN_ACT_SEND_DISCONNECT_REQ, 0},
    {"2-16 N1", SCS_CONN_DISC_SENT, SCS_CONN_EV_RCV_DISCONNECT_RSP,
     SCS_CONN_DISC_ACK, SCS_CONN_ACT_NONE, 0},
    {"2-16 N1", SCS_CONN_DISC_ACK, SCS_CONN_EV_RCV_DISCONNECT_REQ,
     SCS_CONN_CLOSED, SCS_CONN_ACT_SEND_DISCONNECT_RSP, 0},
    /* --- Figure 2-16, NODE_2 --- */
    {"2-16 N2", SCS_CONN_OPEN, SCS_CONN_EV_RCV_DISCONNECT_REQ,
     SCS_CONN_DISC_RECEIVED, SCS_CONN_ACT_SEND_DISCONNECT_RSP,
     SCS_CONN_NOTIFY_DISCONNECTED},
    {"2-16 N2", SCS_CONN_DISC_RECEIVED, SCS_CONN_EV_SVC_DISCONNECT,
     SCS_CONN_DISC_MATCH, SCS_CONN_ACT_SEND_DISCONNECT_REQ, 0},
    {"2-16 N2", SCS_CONN_DISC_MATCH, SCS_CONN_EV_RCV_DISCONNECT_RSP,
     SCS_CONN_CLOSED, SCS_CONN_ACT_NONE, 0},
    /* --- p. 2-27, simultaneous DISCONNECT --- */
    {"p2-27", SCS_CONN_DISC_SENT, SCS_CONN_EV_RCV_DISCONNECT_REQ,
     SCS_CONN_DISC_MATCH, SCS_CONN_ACT_SEND_DISCONNECT_RSP, 0}
};
#define FIGURE_ROWS ((unsigned)(sizeof(figure_rows) / sizeof(figure_rows[0])))

static void test_every_figure_transition_is_present(void)
{
    for (unsigned i = 0; i < FIGURE_ROWS; i++) {
        const struct figure_row *f = &figure_rows[i];
        struct scs_conn_transition t;
        int found = scs_conn_table_lookup(f->from, f->ev, &t);
        CHECK(found, "Fig %s: no rule for %s + %s", f->fig,
              scs_conn_state_name(f->from), scs_conn_event_name(f->ev));
        if (!found) {
            continue;
        }
        CHECK(t.to == f->to, "Fig %s: %s + %s -> %s, figure says %s", f->fig,
              scs_conn_state_name(f->from), scs_conn_event_name(f->ev),
              scs_conn_state_name(t.to), scs_conn_state_name(f->to));
        CHECK(t.action == f->action, "Fig %s: %s + %s action %s, figure says %s", f->fig,
              scs_conn_state_name(f->from), scs_conn_event_name(f->ev),
              scs_conn_action_name(t.action), scs_conn_action_name(f->action));
        CHECK(t.notify == f->notify, "Fig %s: %s + %s notify 0x%02x, figure says 0x%02x",
              f->fig, scs_conn_state_name(f->from), scs_conn_event_name(f->ev),
              t.notify, f->notify);
        CHECK(t.documented == 1, "Fig %s: %s + %s is drawn in the book but is not"
              " flagged documented", f->fig, scs_conn_state_name(f->from),
              scs_conn_event_name(f->ev));
        CHECK(t.illegal == 0, "Fig %s: a drawn transition is flagged illegal", f->fig);
    }
}

/*
 * The converse: every documented row in the production table is one of the
 * figure rows above. Without this, someone could add an undocumented rule,
 * flag it `documented`, and the first test would never notice.
 */
static void test_no_extra_documented_rows(void)
{
    unsigned n = scs_conn_table_size();
    unsigned documented = 0;
    for (unsigned i = 0; i < n; i++) {
        enum scs_conn_event ev;
        struct scs_conn_transition t;
        if (!scs_conn_table_row(i, &ev, &t)) {
            CHECK(0, "row %u unreadable", i);
            continue;
        }
        if (!t.documented) {
            continue;
        }
        documented++;
        int matched = 0;
        for (unsigned j = 0; j < FIGURE_ROWS; j++) {
            if (figure_rows[j].from == t.from && figure_rows[j].ev == ev) {
                matched = 1;
                break;
            }
        }
        CHECK(matched, "table row %s + %s claims to be documented but is not in"
              " Figure 2-14/2-15/2-16", scs_conn_state_name(t.from),
              scs_conn_event_name(ev));
    }
    CHECK(documented == FIGURE_ROWS, "table has %u documented rows, the figures have %u",
          documented, FIGURE_ROWS);
}

/*
 * Every UNdocumented row must be one of the OVMX additions scs_conn.h names. A
 * new unlabeled shortcut must fail this test, not slip in.
 */
static void test_ovmx_rows_are_exactly_the_four_declared(void)
{
    static const struct {
        enum scs_conn_state from;
        enum scs_conn_event ev;
    } expected[] = {
        {SCS_CONN_CONNECT_SENT, SCS_CONN_EV_SVC_CONNECT},      /* retransmit */
        {SCS_CONN_CONNECT_REC, SCS_CONN_EV_RCV_CONNECT_REQ},   /* retransmit */
        {SCS_CONN_ACCEPT_SENT, SCS_CONN_EV_RCV_CONNECT_REQ},   /* retransmit */
        {SCS_CONN_CONNECT_SENT, SCS_CONN_EV_RCV_ACCEPT_REQ},   /* missing 0x4b CONNECT_RSP */
        /* vms-abc: VC loss, one row per state (p. 2-31 states the OUTCOME --
         * the connection is broken and its SYSAP notified -- but no figure
         * draws the arrow, so the rows are OVMX additions). */
        {SCS_CONN_CLOSED, SCS_CONN_EV_VC_LOST},
        {SCS_CONN_CONNECT_SENT, SCS_CONN_EV_VC_LOST},
        {SCS_CONN_CONNECT_ACK, SCS_CONN_EV_VC_LOST},
        {SCS_CONN_CONNECT_REC, SCS_CONN_EV_VC_LOST},
        {SCS_CONN_ACCEPT_SENT, SCS_CONN_EV_VC_LOST},
        {SCS_CONN_REJECT_SENT, SCS_CONN_EV_VC_LOST},
        {SCS_CONN_OPEN, SCS_CONN_EV_VC_LOST},
        {SCS_CONN_DISC_SENT, SCS_CONN_EV_VC_LOST},
        {SCS_CONN_DISC_ACK, SCS_CONN_EV_VC_LOST},
        {SCS_CONN_DISC_RECEIVED, SCS_CONN_EV_VC_LOST},
        {SCS_CONN_DISC_MATCH, SCS_CONN_EV_VC_LOST}
    };
    const unsigned nexp = (unsigned)(sizeof(expected) / sizeof(expected[0]));

    unsigned undocumented = 0;
    for (unsigned i = 0; i < scs_conn_table_size(); i++) {
        enum scs_conn_event ev;
        struct scs_conn_transition t;
        if (!scs_conn_table_row(i, &ev, &t) || t.documented) {
            continue;
        }
        undocumented++;
        int matched = 0;
        for (unsigned j = 0; j < nexp; j++) {
            if (expected[j].from == t.from && expected[j].ev == ev) {
                matched = 1;
                break;
            }
        }
        CHECK(matched, "undeclared OVMX row %s + %s -- every non-figure row must be"
              " listed and justified in scs_conn.h", scs_conn_state_name(t.from),
              scs_conn_event_name(ev));
    }
    CHECK(undocumented == nexp, "%u undocumented rows, scs_conn.h declares %u",
          undocumented, nexp);

    /* The three retransmit rows must not move the state -- that is the whole
     * claim made for them. */
    for (unsigned j = 0; j < 3; j++) {
        struct scs_conn_transition t;
        CHECK(scs_conn_table_lookup(expected[j].from, expected[j].ev, &t),
              "retransmit row %u missing", j);
        CHECK(t.to == t.from, "retransmit row %s + %s moves the state to %s",
              scs_conn_state_name(t.from), scs_conn_event_name(expected[j].ev),
              scs_conn_state_name(t.to));
    }
}

/*
 * vms-abc: the VC-loss rows, walked state by state INDEPENDENTLY of the list
 * above. p. 2-31: "every connection supported by this virtual circuit is also
 * broken, and the SYSAPs participating in these connections are notified of the
 * event." So from EVERY state:
 *   - a rule must exist (a VC loss is never an illegal event),
 *   - it must land in CLOSED,
 *   - it must ask for NO packet (the circuit is gone -- p. 2-31: "Any attempt
 *     to send a message from one port to another in the absence of a virtual
 *     circuit will fail"),
 *   - and every state that HAD a connection must notify the SYSAP.
 * A mutant that drops one row, sends a packet, or forgets the notify bit dies
 * here.
 */
static void test_vc_loss_closes_every_state_and_notifies(void)
{
    for (int s = 0; s < SCS_CONN_STATE_COUNT; s++) {
        enum scs_conn_state from = (enum scs_conn_state)s;
        struct scs_conn_transition t;
        CHECK(scs_conn_table_lookup(from, SCS_CONN_EV_VC_LOST, &t),
              "no VC-loss rule for state %s -- a VC loss must never be an"
              " illegal event", scs_conn_state_name(from));
        CHECK(!t.illegal, "VC loss in %s is illegal", scs_conn_state_name(from));
        CHECK(t.to == SCS_CONN_CLOSED, "VC loss in %s lands in %s, not CLOSED",
              scs_conn_state_name(from), scs_conn_state_name(t.to));
        CHECK(t.action == SCS_CONN_ACT_NONE,
              "VC loss in %s asks to send '%s' into a circuit that is gone",
              scs_conn_state_name(from), scs_conn_action_name(t.action));
        CHECK(t.documented == 0,
              "the VC-loss row for %s claims to be drawn in a figure; no figure"
              " draws a VC-loss arc", scs_conn_state_name(from));
        if (from == SCS_CONN_CLOSED) {
            CHECK(t.notify == 0, "VC loss in CLOSED notifies a SYSAP about a"
                                 " connection that was never formed");
        } else {
            CHECK((t.notify & SCS_CONN_NOTIFY_DISCONNECTED) != 0,
                  "VC loss in %s does not notify the SYSAP (p. 2-31 requires it)",
                  scs_conn_state_name(from));
        }
    }

    /* And it must actually MOVE a live connection, through the machine. */
    struct scs_cdt *c = fresh_conn("VMS$VAXcluster", "VMS$VAXcluster");
    CHECK(c != NULL, "allocation failed");
    if (c == NULL) {
        return;
    }
    (void)scs_conn_fsm_step(c, SCS_CONN_EV_SVC_CONNECT);
    (void)scs_conn_fsm_step(c, SCS_CONN_EV_RCV_CONNECT_RSP);
    (void)scs_conn_fsm_step(c, SCS_CONN_EV_RCV_ACCEPT_REQ);
    CHECK_STATE(c, SCS_CONN_OPEN);
    struct scs_conn_transition t = scs_conn_fsm_step(c, SCS_CONN_EV_VC_LOST);
    CHECK(t.from == SCS_CONN_OPEN && t.to == SCS_CONN_CLOSED,
          "an OPEN connection did not go to CLOSED on VC loss");
    CHECK_STATE(c, SCS_CONN_CLOSED);
}

/* ==========================================================================
 * 2. THE THREE PATHS, walked live, both ends, asserting state after EVERY
 *    message in EACH direction.
 * ========================================================================== */

/* Figure 2-14 NODE_1: the source SYSAP's side of a successful formation. */
static void test_formation_path_source(void)
{
    struct scs_cdt *c = fresh_conn("VMS$VAXcluster", "VMS$VAXcluster");
    CHECK(c != NULL, "allocation failed");
    if (c == NULL) {
        return;
    }
    CHECK_STATE(c, SCS_CONN_CLOSED); /* "o CONN STATE = 'CLOSED'" */

    struct scs_conn_transition t = scs_conn_fsm_step(c, SCS_CONN_EV_SVC_CONNECT);
    CHECK(t.action == SCS_CONN_ACT_SEND_CONNECT_REQ, "CONNECT must send CONNECT_REQ");
    CHECK_STATE(c, SCS_CONN_CONNECT_SENT);

    t = scs_conn_fsm_step(c, SCS_CONN_EV_RCV_CONNECT_RSP);
    CHECK(t.action == SCS_CONN_ACT_NONE, "CONNECT_RSP sends nothing back");
    CHECK_STATE(c, SCS_CONN_CONNECT_ACK);

    t = scs_conn_fsm_step(c, SCS_CONN_EV_RCV_ACCEPT_REQ);
    CHECK(t.action == SCS_CONN_ACT_SEND_ACCEPT_RSP, "ACCEPT_REQ must be answered ACCEPT_RSP");
    CHECK((t.notify & SCS_CONN_NOTIFY_ACCEPTED) != 0, "source SYSAP must be told about acceptance");
    CHECK_STATE(c, SCS_CONN_OPEN);
}

/* Figure 2-14 NODE_2: the target SYSAP's side of the same formation. */
static void test_formation_path_target(void)
{
    struct scs_cdt *c = fresh_conn("SCS$DIRECTORY", "SCS$DIRECTORY");
    CHECK(c != NULL, "allocation failed");
    if (c == NULL) {
        return;
    }
    CHECK_STATE(c, SCS_CONN_CLOSED);

    struct scs_conn_transition t = scs_conn_fsm_step(c, SCS_CONN_EV_RCV_CONNECT_REQ);
    CHECK(t.action == SCS_CONN_ACT_SEND_CONNECT_RSP, "CONNECT_REQ must be acked CONNECT_RSP");
    CHECK((t.notify & SCS_CONN_NOTIFY_CONNECT_REQ) != 0, "request must reach the target SYSAP");
    CHECK((t.notify & SCS_CONN_NOTIFY_LISTEN_STOP) != 0,
          "p. 2-23: the target SYSAP temporarily stops listening");
    CHECK_STATE(c, SCS_CONN_CONNECT_REC);

    t = scs_conn_fsm_step(c, SCS_CONN_EV_SVC_ACCEPT);
    CHECK(t.action == SCS_CONN_ACT_SEND_ACCEPT_REQ, "ACCEPT must send ACCEPT_REQ");
    CHECK_STATE(c, SCS_CONN_ACCEPT_SENT);

    t = scs_conn_fsm_step(c, SCS_CONN_EV_RCV_ACCEPT_RSP);
    CHECK(t.action == SCS_CONN_ACT_NONE, "ACCEPT_RSP sends nothing back");
    CHECK((t.notify & SCS_CONN_NOTIFY_LISTEN_RESUME) != 0,
          "'TARGET SYSAP AGAIN LISTENING'");
    CHECK_STATE(c, SCS_CONN_OPEN);
}

/* Figure 2-15: rejection, both ends. */
static void test_rejection_path(void)
{
    /* NODE_2, the rejecting target. */
    struct scs_cdt *c = fresh_conn("SCS$DIRECTORY", "SCS$DIRECTORY");
    CHECK(c != NULL, "allocation failed");
    if (c == NULL) {
        return;
    }
    (void)scs_conn_fsm_step(c, SCS_CONN_EV_RCV_CONNECT_REQ);
    CHECK_STATE(c, SCS_CONN_CONNECT_REC);

    struct scs_conn_transition t = scs_conn_fsm_step(c, SCS_CONN_EV_SVC_REJECT);
    CHECK(t.action == SCS_CONN_ACT_SEND_REJECT_REQ, "REJECT must send REJECT_REQ");
    CHECK_STATE(c, SCS_CONN_REJECT_SENT);

    t = scs_conn_fsm_step(c, SCS_CONN_EV_RCV_REJECT_RSP);
    CHECK((t.notify & SCS_CONN_NOTIFY_LISTEN_RESUME) != 0,
          "p. 2-24: the target SYSAP resumes listening");
    CHECK_STATE(c, SCS_CONN_CLOSED);

    /* NODE_1, the rejected source. Same CDL, fresh CDT. */
    struct scs_cdt *s = fresh_conn("VMS$VAXcluster", "VMS$VAXcluster");
    CHECK(s != NULL, "allocation failed");
    if (s == NULL) {
        return;
    }
    (void)scs_conn_fsm_step(s, SCS_CONN_EV_SVC_CONNECT);
    (void)scs_conn_fsm_step(s, SCS_CONN_EV_RCV_CONNECT_RSP);
    CHECK_STATE(s, SCS_CONN_CONNECT_ACK);

    t = scs_conn_fsm_step(s, SCS_CONN_EV_RCV_REJECT_REQ);
    CHECK(t.action == SCS_CONN_ACT_SEND_REJECT_RSP,
          "p. 2-24: the source sends a REJECT_RSP back");
    CHECK((t.notify & SCS_CONN_NOTIFY_REJECTED) != 0, "source SYSAP must be told");
    CHECK_STATE(s, SCS_CONN_CLOSED); /* "SCS ABANDONS CONNECTION" */
}

/* Figure 2-16: the four-message explicit disconnect, both ends. */
static void test_disconnect_path(void)
{
    /* NODE_1: the side whose SYSAP disconnects first. */
    struct scs_cdt *n1 = fresh_conn("VMS$VAXcluster", "VMS$VAXcluster");
    CHECK(n1 != NULL, "allocation failed");
    if (n1 == NULL) {
        return;
    }
    n1->conn_state = (int)SCS_CONN_OPEN;

    struct scs_conn_transition t = scs_conn_fsm_step(n1, SCS_CONN_EV_SVC_DISCONNECT);
    CHECK(t.action == SCS_CONN_ACT_SEND_DISCONNECT_REQ, "DISCONNECT must send DISCONNECT_REQ");
    CHECK_STATE(n1, SCS_CONN_DISC_SENT);

    t = scs_conn_fsm_step(n1, SCS_CONN_EV_RCV_DISCONNECT_RSP);
    CHECK(t.action == SCS_CONN_ACT_NONE, "DISCONNECT_RSP sends nothing back");
    CHECK_STATE(n1, SCS_CONN_DISC_ACK);

    t = scs_conn_fsm_step(n1, SCS_CONN_EV_RCV_DISCONNECT_REQ);
    CHECK(t.action == SCS_CONN_ACT_SEND_DISCONNECT_RSP,
          "the matching DISCONNECT_REQ is answered with a matching DISCONNECT_RSP");
    CHECK_STATE(n1, SCS_CONN_CLOSED);

    /* NODE_2: the side that answers, then symmetrically disconnects. */
    struct scs_cdt *n2 = fresh_conn("VMS$VAXcluster", "VMS$VAXcluster");
    CHECK(n2 != NULL, "allocation failed");
    if (n2 == NULL) {
        return;
    }
    n2->conn_state = (int)SCS_CONN_OPEN;

    t = scs_conn_fsm_step(n2, SCS_CONN_EV_RCV_DISCONNECT_REQ);
    CHECK(t.action == SCS_CONN_ACT_SEND_DISCONNECT_RSP, "must answer DISCONNECT_RSP");
    CHECK((t.notify & SCS_CONN_NOTIFY_DISCONNECTED) != 0, "SYSAP must be notified");
    CHECK_STATE(n2, SCS_CONN_DISC_RECEIVED);

    t = scs_conn_fsm_step(n2, SCS_CONN_EV_SVC_DISCONNECT);
    CHECK(t.action == SCS_CONN_ACT_SEND_DISCONNECT_REQ, "the matching DISCONNECT_REQ");
    CHECK_STATE(n2, SCS_CONN_DISC_MATCH);

    t = scs_conn_fsm_step(n2, SCS_CONN_EV_RCV_DISCONNECT_RSP);
    CHECK(t.action == SCS_CONN_ACT_NONE, "nothing more goes out");
    CHECK_STATE(n2, SCS_CONN_CLOSED);
}

/*
 * p. 2-27: "both SYSAPs could simultaneously invoke the DISCONNECT service ...
 * both sides of the connection symmetrically transitioning through only three
 * states". Asserted as THREE states, not just a final CLOSED: the count is the
 * claim the book makes.
 */
static void test_simultaneous_disconnect_is_three_states(void)
{
    struct scs_cdt *c = fresh_conn("VMS$VAXcluster", "VMS$VAXcluster");
    CHECK(c != NULL, "allocation failed");
    if (c == NULL) {
        return;
    }
    c->conn_state = (int)SCS_CONN_OPEN;

    (void)scs_conn_fsm_step(c, SCS_CONN_EV_SVC_DISCONNECT);
    CHECK_STATE(c, SCS_CONN_DISC_SENT);

    struct scs_conn_transition t = scs_conn_fsm_step(c, SCS_CONN_EV_RCV_DISCONNECT_REQ);
    CHECK(t.action == SCS_CONN_ACT_SEND_DISCONNECT_RSP, "it replies with a DISCONNECT_RSP");
    CHECK_STATE(c, SCS_CONN_DISC_MATCH); /* NOT DISC ACK -- it saw a matching REQ */

    (void)scs_conn_fsm_step(c, SCS_CONN_EV_RCV_DISCONNECT_RSP);
    CHECK_STATE(c, SCS_CONN_CLOSED);
    /* Three states after OPEN: DISC SENT, DISC MATCH, CLOSED. DISC ACK and
     * DISC RECEIVED are never entered on this path. */
}

/* Every state named in the figures must be reachable and distinctly named. */
static void test_all_eleven_states_exist_and_are_named(void)
{
    static const enum scs_conn_state all[] = {
        SCS_CONN_CLOSED, SCS_CONN_CONNECT_SENT, SCS_CONN_CONNECT_ACK,
        SCS_CONN_CONNECT_REC, SCS_CONN_ACCEPT_SENT, SCS_CONN_REJECT_SENT,
        SCS_CONN_OPEN, SCS_CONN_DISC_SENT, SCS_CONN_DISC_ACK,
        SCS_CONN_DISC_RECEIVED, SCS_CONN_DISC_MATCH
    };
    const unsigned n = (unsigned)(sizeof(all) / sizeof(all[0]));
    CHECK(n == SCS_CONN_STATE_COUNT, "%u states listed, SCS_CONN_STATE_COUNT is %d",
          n, SCS_CONN_STATE_COUNT);
    for (unsigned i = 0; i < n; i++) {
        const char *ni = scs_conn_state_name(all[i]);
        CHECK(ni[0] != '?', "state %u has no name", (unsigned)all[i]);
        for (unsigned j = i + 1; j < n; j++) {
            CHECK(strcmp(ni, scs_conn_state_name(all[j])) != 0,
                  "states %u and %u share the name '%s'", (unsigned)all[i],
                  (unsigned)all[j], ni);
        }
    }
    /* Every state must be REACHED by at least one table row (except CLOSED,
     * which is the initial state). */
    for (unsigned i = 0; i < n; i++) {
        if (all[i] == SCS_CONN_CLOSED) {
            continue;
        }
        int reached = 0;
        for (unsigned r = 0; r < scs_conn_table_size(); r++) {
            enum scs_conn_event ev;
            struct scs_conn_transition t;
            if (scs_conn_table_row(r, &ev, &t) && t.to == all[i] && t.to != t.from) {
                reached = 1;
                break;
            }
        }
        CHECK(reached, "state %s is unreachable -- no transition enters it",
              scs_conn_state_name(all[i]));
    }
}

/*
 * An event with no rule for the current state leaves the state ALONE and is
 * reported illegal. That is what makes a stalled connection legible rather than
 * silently corrupted.
 */
static void test_illegal_events_do_not_move_the_state(void)
{
    struct scs_cdt *c = fresh_conn("VMS$VAXcluster", "VMS$VAXcluster");
    CHECK(c != NULL, "allocation failed");
    if (c == NULL) {
        return;
    }
    /* An ACCEPT_RSP in CLOSED: nothing was ever accepted. */
    struct scs_conn_transition t = scs_conn_fsm_step(c, SCS_CONN_EV_RCV_ACCEPT_RSP);
    CHECK(t.illegal == 1, "ACCEPT_RSP in CLOSED must be illegal");
    CHECK_STATE(c, SCS_CONN_CLOSED);

    /* A DISCONNECT_RSP in OPEN: nothing was ever disconnected. */
    c->conn_state = (int)SCS_CONN_OPEN;
    t = scs_conn_fsm_step(c, SCS_CONN_EV_RCV_DISCONNECT_RSP);
    CHECK(t.illegal == 1, "DISCONNECT_RSP in OPEN must be illegal");
    CHECK_STATE(c, SCS_CONN_OPEN);

    /* An ACCEPT_RSP arriving twice: the second is illegal, and OPEN holds. */
    struct scs_cdt *d = fresh_conn("SCS$DIRECTORY", "SCS$DIRECTORY");
    CHECK(d != NULL, "allocation failed");
    if (d == NULL) {
        return;
    }
    (void)scs_conn_fsm_step(d, SCS_CONN_EV_RCV_CONNECT_REQ);
    (void)scs_conn_fsm_step(d, SCS_CONN_EV_SVC_ACCEPT);
    (void)scs_conn_fsm_step(d, SCS_CONN_EV_RCV_ACCEPT_RSP);
    CHECK_STATE(d, SCS_CONN_OPEN);
    t = scs_conn_fsm_step(d, SCS_CONN_EV_RCV_ACCEPT_RSP);
    CHECK(t.illegal == 1, "a duplicate ACCEPT_RSP in OPEN must be illegal");
    CHECK_STATE(d, SCS_CONN_OPEN);

    /* NULL is tolerated and reported illegal, never dereferenced. */
    t = scs_conn_fsm_step(NULL, SCS_CONN_EV_SVC_CONNECT);
    CHECK(t.illegal == 1, "a NULL CDT must be reported illegal");
    CHECK(t.suppressed == 0, "a NULL CDT is not the kill switch");
}

/* The table is TOTAL: every (state, event) pair answers without reading memory
 * it should not. Also pins that a lookup never mutates a CDT. */
static void test_table_is_total(void)
{
    unsigned covered = 0;
    for (int s = 0; s < SCS_CONN_STATE_COUNT; s++) {
        for (int e = 0; e < SCS_CONN_EVENT_COUNT; e++) {
            struct scs_conn_transition t;
            int found = scs_conn_table_lookup((enum scs_conn_state)s,
                                              (enum scs_conn_event)e, &t);
            CHECK(t.from == (enum scs_conn_state)s, "lookup lost the from-state");
            if (found) {
                covered++;
                CHECK(t.illegal == 0, "a found rule must not be illegal");
            } else {
                CHECK(t.illegal == 1, "an absent rule must be illegal");
                CHECK(t.to == (enum scs_conn_state)s, "an illegal event must not move state");
                CHECK(t.action == SCS_CONN_ACT_NONE, "an illegal event must send nothing");
            }
        }
    }
    CHECK(covered == scs_conn_table_size(),
          "%u of the %u*%u pairs are covered, the table has %u rows", covered,
          SCS_CONN_STATE_COUNT, SCS_CONN_EVENT_COUNT, scs_conn_table_size());
}

/* ==========================================================================
 * 3. LOGGING -- "each transition is logged with the CONID".
 * ========================================================================== */
static void test_transition_log_carries_the_conid(void)
{
    char  *buf = NULL;
    size_t len = 0;
    FILE  *mem = open_memstream(&buf, &len);
    CHECK(mem != NULL, "open_memstream failed");
    if (mem == NULL) {
        return;
    }

    scs_cdl_init(&cdl);
    /* Claim the Con.ID SCSD actually puts on the wire for SCS$DIRECTORY, so the
     * string this test greps for is the string a real run log contains. */
    struct scs_cdt *c = scs_cdl_alloc_conid(&cdl, 0x4F580007u, "SCS$DIRECTORY",
                                            "SCS$DIRECTORY", NULL);
    CHECK(c != NULL, "conid allocation failed");
    if (c == NULL) {
        (void)fclose(mem);
        free(buf);
        return;
    }
    scs_conn_fsm_init(c);
    scs_cdt_set_remote_conid(c, 0x63050008u);

    scs_conn_set_log(mem);
    (void)scs_conn_fsm_step(c, SCS_CONN_EV_RCV_CONNECT_REQ);
    (void)scs_conn_fsm_step(c, SCS_CONN_EV_SVC_ACCEPT);
    /* An illegal event must be the LOUDEST line, not a silent one. */
    (void)scs_conn_fsm_step(c, SCS_CONN_EV_RCV_DISCONNECT_RSP);
    scs_conn_set_log(NULL);
    (void)fclose(mem);

    CHECK(buf != NULL && strstr(buf, "0x4F580007") != NULL,
          "the transition log does not carry the local CONID");
    CHECK(buf != NULL && strstr(buf, "0x63050008") != NULL,
          "the transition log does not carry the remote CONID");
    CHECK(buf != NULL && strstr(buf, "CLOSED --RCV_CONNECT_REQ--> CONNECT REC") != NULL,
          "the first transition is not in the log");
    CHECK(buf != NULL && strstr(buf, "CONNECT REC --SVC_ACCEPT--> ACCEPT SENT") != NULL,
          "the second transition is not in the log");
    CHECK(buf != NULL && strstr(buf, "ILLEGAL-EVENT-FOR-THIS-STATE") != NULL,
          "an illegal event was not marked in the log");
    CHECK(buf != NULL && strstr(buf, "SCS$DIRECTORY") != NULL,
          "the log does not name the SYSAPs");
    free(buf);
}

/* An OVMX (undocumented) row must SAY SO in the log, so a run log cannot be
 * read as if every transition came out of the book. */
static void test_ovmx_row_is_labeled_in_the_log(void)
{
    char  *buf = NULL;
    size_t len = 0;
    FILE  *mem = open_memstream(&buf, &len);
    CHECK(mem != NULL, "open_memstream failed");
    if (mem == NULL) {
        return;
    }
    struct scs_cdt *c = fresh_conn("VMS$VAXcluster", "VMS$VAXcluster");
    CHECK(c != NULL, "allocation failed");
    if (c == NULL) {
        (void)fclose(mem);
        free(buf);
        return;
    }
    scs_conn_set_log(mem);
    (void)scs_conn_fsm_step(c, SCS_CONN_EV_SVC_CONNECT);       /* documented */
    (void)scs_conn_fsm_step(c, SCS_CONN_EV_RCV_ACCEPT_REQ);    /* the OVMX row */
    scs_conn_set_log(NULL);
    (void)fclose(mem);

    CHECK_STATE(c, SCS_CONN_OPEN);
    CHECK(buf != NULL && strstr(buf, "[OVMX row, not in Fig 2-14/15/16]") != NULL,
          "an undocumented transition was logged as if it were in the book");
    /* And the documented one must NOT carry that label. */
    const char *first = buf ? strstr(buf, "CLOSED --SVC_CONNECT-->") : NULL;
    CHECK(first != NULL, "the documented transition is missing from the log");
    if (first != NULL) {
        const char *eol = strchr(first, '\n');
        CHECK(eol != NULL && (strstr(first, "[OVMX row") == NULL ||
                              strstr(first, "[OVMX row") > eol),
              "a documented transition was labeled as an OVMX row");
    }
    free(buf);
}

/* ==========================================================================
 * 4. THE KILL SWITCH -- RUN, not described (guardrail 23).
 * ========================================================================== */
static void test_kill_switch_suppresses_exactly_what_it_claims(void)
{
    /* Baseline: the switch UNSET. The state moves and the log is written. */
    CHECK(unsetenv("OVMX_NO_CONN_FSM") == 0, "unsetenv failed");
    CHECK(scs_conn_fsm_enabled() == 1, "machine should be enabled with the var unset");

    char  *on_buf = NULL;
    size_t on_len = 0;
    FILE  *on_mem = open_memstream(&on_buf, &on_len);
    struct scs_cdt *c = fresh_conn("VMS$VAXcluster", "VMS$VAXcluster");
    CHECK(c != NULL && on_mem != NULL, "setup failed");
    if (c == NULL || on_mem == NULL) {
        return;
    }
    scs_conn_set_log(on_mem);
    struct scs_conn_transition t = scs_conn_fsm_step(c, SCS_CONN_EV_SVC_CONNECT);
    scs_conn_set_log(NULL);
    (void)fclose(on_mem);
    CHECK(t.suppressed == 0, "not suppressed with the switch unset");
    CHECK_STATE(c, SCS_CONN_CONNECT_SENT);
    CHECK(on_len > 0, "nothing was logged with the switch unset");

    /* Now SET it, and re-run the SAME step from the SAME state. */
    CHECK(setenv("OVMX_NO_CONN_FSM", "1", 1) == 0, "setenv failed");
    CHECK(scs_conn_fsm_enabled() == 0, "machine should be disabled with the var set");

    char  *off_buf = NULL;
    size_t off_len = 0;
    FILE  *off_mem = open_memstream(&off_buf, &off_len);
    struct scs_cdt *d = fresh_conn("VMS$VAXcluster", "VMS$VAXcluster");
    CHECK(d != NULL && off_mem != NULL, "setup failed");
    if (d == NULL || off_mem == NULL) {
        free(on_buf);
        return;
    }
    scs_conn_set_log(off_mem);
    t = scs_conn_fsm_step(d, SCS_CONN_EV_SVC_CONNECT);
    scs_conn_set_log(NULL);
    (void)fclose(off_mem);

    CHECK(t.suppressed == 1, "the switch did not mark the step suppressed");
    CHECK_STATE(d, SCS_CONN_CLOSED); /* the state did NOT move */
    CHECK(off_len == 0, "the switch did not silence the transition log (%zu bytes)", off_len);

    /* And the diagnostic must refuse to say "0 stuck" while disabled. */
    char  *rep = NULL;
    size_t rlen = 0;
    FILE  *rmem = open_memstream(&rep, &rlen);
    if (rmem != NULL) {
        (void)scs_conn_report_stuck(&cdl, rmem);
        (void)fclose(rmem);
        CHECK(rep != NULL && strstr(rep, "DISABLED") != NULL,
              "the stuck report did not say the machine was disabled");
        CHECK(rep != NULL && strstr(rep, "SCSD-W-CONNSTUCK") == NULL,
              "the stuck report named connections while disabled");
        free(rep);
    }

    /* "0" is explicitly NOT off -- a switch that fires on OVMX_NO_CONN_FSM=0
     * would be a trap. */
    CHECK(setenv("OVMX_NO_CONN_FSM", "0", 1) == 0, "setenv failed");
    CHECK(scs_conn_fsm_enabled() == 1, "OVMX_NO_CONN_FSM=0 must NOT disable the machine");

    CHECK(unsetenv("OVMX_NO_CONN_FSM") == 0, "unsetenv failed");
    free(on_buf);
    free(off_buf);
}

/* ==========================================================================
 * 5. THE STUCK DIAGNOSTIC.
 * ========================================================================== */
static void test_stuck_connections_are_reported_by_conid(void)
{
    scs_cdl_init(&cdl);
    struct scs_cdt *open_c = scs_cdl_alloc_conid(&cdl, 0x4F580001u, "VMS$VAXcluster",
                                                 "VMS$VAXcluster", NULL);
    struct scs_cdt *parked = scs_cdl_alloc_conid(&cdl, 0x4F580002u, "VMS$VAXcluster",
                                                 "VMS$VAXcluster", NULL);
    struct scs_cdt *halfdisc = scs_cdl_alloc_conid(&cdl, 0x4F580007u, "SCS$DIRECTORY",
                                                   "SCS$DIRECTORY", NULL);
    CHECK(open_c != NULL && parked != NULL && halfdisc != NULL, "allocation failed");
    if (open_c == NULL || parked == NULL || halfdisc == NULL) {
        return;
    }
    scs_conn_fsm_init(open_c);
    scs_conn_fsm_init(parked);
    scs_conn_fsm_init(halfdisc);

    /* One reaches OPEN. */
    (void)scs_conn_fsm_step(open_c, SCS_CONN_EV_RCV_CONNECT_REQ);
    (void)scs_conn_fsm_step(open_c, SCS_CONN_EV_SVC_ACCEPT);
    (void)scs_conn_fsm_step(open_c, SCS_CONN_EV_RCV_ACCEPT_RSP);
    CHECK_STATE(open_c, SCS_CONN_OPEN);

    /* One never gets its ACCEPT_RSP -- the shape the 0x4b class shows today. */
    (void)scs_conn_fsm_step(parked, SCS_CONN_EV_RCV_CONNECT_REQ);
    (void)scs_conn_fsm_step(parked, SCS_CONN_EV_SVC_ACCEPT);
    CHECK_STATE(parked, SCS_CONN_ACCEPT_SENT);

    /* One is parked mid-disconnect -- the shape HANDOFF-vms-2f3 sec 4M.18/4M.28
     * records on the PEER side (a CDT sitting in disc_sent/disc_pend). */
    halfdisc->conn_state = (int)SCS_CONN_OPEN;
    (void)scs_conn_fsm_step(halfdisc, SCS_CONN_EV_SVC_DISCONNECT);
    (void)scs_conn_fsm_step(halfdisc, SCS_CONN_EV_RCV_DISCONNECT_RSP);
    CHECK_STATE(halfdisc, SCS_CONN_DISC_ACK);

    char  *rep = NULL;
    size_t rlen = 0;
    FILE  *rmem = open_memstream(&rep, &rlen);
    CHECK(rmem != NULL, "open_memstream failed");
    if (rmem == NULL) {
        return;
    }
    unsigned stuck = scs_conn_report_stuck(&cdl, rmem);
    (void)fclose(rmem);

    CHECK(stuck == 2, "reported %u stuck, expected 2", stuck);
    CHECK(rep != NULL && strstr(rep, "0x4F580002") != NULL,
          "the ACCEPT SENT connection was not named by CONID");
    CHECK(rep != NULL && strstr(rep, "ACCEPT SENT") != NULL,
          "the parked state was not named");
    CHECK(rep != NULL && strstr(rep, "0x4F580007") != NULL,
          "the DISC ACK connection was not named by CONID");
    CHECK(rep != NULL && strstr(rep, "DISC ACK") != NULL, "DISC ACK was not named");
    CHECK(rep != NULL && strstr(rep, "0x4F580001") == NULL,
          "the OPEN connection was wrongly reported stuck");
    free(rep);

    /* Counting without printing must agree. */
    CHECK(scs_conn_report_stuck(&cdl, NULL) == 2, "silent count disagrees");
    /* A CDL with nothing in it reports nothing. */
    scs_cdl_init(&cdl);
    CHECK(scs_conn_report_stuck(&cdl, NULL) == 0, "an empty CDL reported stuck connections");
    CHECK(scs_conn_report_stuck(NULL, NULL) == 0, "NULL CDL must be tolerated");
}

/* The state really does live in the CDT field vms-e1a reserved for it -- not in
 * a parallel structure the CDL knows nothing about. */
static void test_state_lives_in_the_cdt_conn_state_field(void)
{
    struct scs_cdt *c = fresh_conn("VMS$VAXcluster", "VMS$VAXcluster");
    CHECK(c != NULL, "allocation failed");
    if (c == NULL) {
        return;
    }
    (void)scs_conn_fsm_step(c, SCS_CONN_EV_SVC_CONNECT);
    CHECK(c->conn_state == (int)SCS_CONN_CONNECT_SENT,
          "conn_state is %d, expected %d", c->conn_state, (int)SCS_CONN_CONNECT_SENT);
    /* And it is reachable the way a real receive path would reach it: by CONID
     * through the CDL (p. 2-29). */
    struct scs_cdt *found = scs_cdl_lookup(&cdl, c->local_conid);
    CHECK(found == c, "CONID lookup did not find the CDT the state is in");
    CHECK(found != NULL && scs_conn_state_of(found) == SCS_CONN_CONNECT_SENT,
          "the state is not visible through the CDL");
}

int main(void)
{
    /* Guard against an inherited environment: several tests below assume the
     * machine starts enabled. */
    (void)unsetenv("OVMX_NO_CONN_FSM");

    test_every_figure_transition_is_present();
    test_no_extra_documented_rows();
    test_ovmx_rows_are_exactly_the_four_declared();
    test_vc_loss_closes_every_state_and_notifies();
    test_formation_path_source();
    test_formation_path_target();
    test_rejection_path();
    test_disconnect_path();
    test_simultaneous_disconnect_is_three_states();
    test_all_eleven_states_exist_and_are_named();
    test_illegal_events_do_not_move_the_state();
    test_table_is_total();
    test_transition_log_carries_the_conid();
    test_ovmx_row_is_labeled_in_the_log();
    test_kill_switch_suppresses_exactly_what_it_claims();
    test_stuck_connections_are_reported_by_conid();
    test_state_lives_in_the_cdt_conn_state_field();

    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
