/*
 * scs_conn.c - the SCA connection state machine (vms-dd5). See scs_conn.h for
 * the provenance, the wire verdict, the kill-switch statement and the
 * documented-vs-OVMX rule. This file is the table and three small functions.
 */
#include "scs_conn.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- *
 * THE TABLE.
 *
 * One row per (from, event) pair that any rule covers. Anything absent is an
 * illegal event for that state: the state is left alone and the step is logged.
 * `doc` is 1 when the row is drawn in Figure 2-14, 2-15 or 2-16 or stated in
 * the surrounding narrative, 0 when it is an OVMX addition (rule 8 labeling).
 * ------------------------------------------------------------------------- */
struct conn_rule {
    enum scs_conn_state  from;
    enum scs_conn_event  ev;
    enum scs_conn_state  to;
    enum scs_conn_action action;
    unsigned             notify;
    int                  doc;
};

static const struct conn_rule conn_table[] = {

    /* === Figure 2-14, NODE_1 (the SOURCE: its SYSAP invoked CONNECT) ====== */

    /* "o CONN STATE = 'CLOSED' / o SCS SENDS 'CONNECT_REQ' TO NODE_2 /
     *  o CONN STATE = 'CONNECT SENT'" (Fig 2-14). p. 2-23: "the state of the
     *  connection from NODE_1's point of view now advances to CONNECT SENT." */
    {SCS_CONN_CLOSED, SCS_CONN_EV_SVC_CONNECT,
     SCS_CONN_CONNECT_SENT, SCS_CONN_ACT_SEND_CONNECT_REQ, 0, 1},

    /* p. 2-23: "NODE_1's port driver passes it to SCS code, which advances the
     * state of the connection on NODE_1 to CONNECT ACKNOWLEDGED." The
     * CONNECT_RSP is a bare acknowledgement -- no packet goes back out. */
    {SCS_CONN_CONNECT_SENT, SCS_CONN_EV_RCV_CONNECT_RSP,
     SCS_CONN_CONNECT_ACK, SCS_CONN_ACT_NONE, 0, 1},

    /* "o SCS SENDS 'ACCEPT_RSP' TO NODE_2 / o SCS NOTIFIES SOURCE SYSAP ABOUT
     *  ACCEPTANCE / o CONN STATE = 'OPEN'" (Fig 2-14). p. 2-23: "NODE_1's SCS
     *  code advances the state of the connection ... to OPEN, and notifies the
     *  source SYSAP of this. NODE_1's SCS code also sends an accept response
     *  (ACCEPT_RSP) to NODE_2". */
    {SCS_CONN_CONNECT_ACK, SCS_CONN_EV_RCV_ACCEPT_REQ,
     SCS_CONN_OPEN, SCS_CONN_ACT_SEND_ACCEPT_RSP, SCS_CONN_NOTIFY_ACCEPTED, 1},

    /* === Figure 2-15, NODE_1 (the source, rejected) ======================= */

    /* p. 2-24: "When NODE_1's SCS code receives the reject request, it abandons
     * formation of the connection, sends a reject response (REJECT_RSP) packet
     * to NODE_2's SCS code, and notifies the source SYSAP about the rejection."
     * Figure 2-15 ends NODE_1's column there and never names a state; an
     * abandoned formation is not a connection, so it returns to CLOSED -- the
     * state Figure 2-14 gives a connection before CONNECT is invoked. */
    {SCS_CONN_CONNECT_ACK, SCS_CONN_EV_RCV_REJECT_REQ,
     SCS_CONN_CLOSED, SCS_CONN_ACT_SEND_REJECT_RSP, SCS_CONN_NOTIFY_REJECTED, 1},

    /* === Figure 2-14, NODE_2 (the TARGET: it was "LISTENING") ============= */

    /* "o SCS SENDS 'CONNECT_RSP' TO NODE_1 / o SCS PASSES 'CONNECT_REQ' TO
     *  TARGET SYSAP / o CONN STATE = 'CONNECT REC'" (Fig 2-14). p. 2-23:
     *  "Since a SYSAP can process only one CONNECT_REQ at a time, the target
     *  SYSAP temporarily stops listening for incoming CONNECT_REQs." */
    {SCS_CONN_CLOSED, SCS_CONN_EV_RCV_CONNECT_REQ,
     SCS_CONN_CONNECT_REC, SCS_CONN_ACT_SEND_CONNECT_RSP,
     SCS_CONN_NOTIFY_CONNECT_REQ | SCS_CONN_NOTIFY_LISTEN_STOP, 1},

    /* "o TARGET SYSAP INVOKES ACCEPT / o SCS SENDS 'ACCEPT_REQ' TO NODE_1 /
     *  o CONN STATE = 'ACCEPT SENT'" (Fig 2-14). */
    {SCS_CONN_CONNECT_REC, SCS_CONN_EV_SVC_ACCEPT,
     SCS_CONN_ACCEPT_SENT, SCS_CONN_ACT_SEND_ACCEPT_REQ, 0, 1},

    /* p. 2-23: "The SCS code sets the state of the connection to OPEN relative
     * to NODE_2. The target SYSAP also resumes listening for more incoming
     * CONNECT_REQs." */
    {SCS_CONN_ACCEPT_SENT, SCS_CONN_EV_RCV_ACCEPT_RSP,
     SCS_CONN_OPEN, SCS_CONN_ACT_NONE, SCS_CONN_NOTIFY_LISTEN_RESUME, 1},

    /* === Figure 2-15, NODE_2 (the target, rejecting) ====================== */

    /* p. 2-24: "SCS code in NODE_2 builds and sends a reject request
     * (REJECT_REQ) packet to the SCS code in NODE_1. The state of the
     * connection from NODE_2's point of view is set to the REJECT SENT state." */
    {SCS_CONN_CONNECT_REC, SCS_CONN_EV_SVC_REJECT,
     SCS_CONN_REJECT_SENT, SCS_CONN_ACT_SEND_REJECT_REQ, 0, 1},

    /* p. 2-24: "When the reject response is received by NODE_2's SCS code, the
     * target SYSAP resumes listening for incoming connect requests." The
     * connection was never formed, so it is CLOSED. */
    {SCS_CONN_REJECT_SENT, SCS_CONN_EV_RCV_REJECT_RSP,
     SCS_CONN_CLOSED, SCS_CONN_ACT_NONE, SCS_CONN_NOTIFY_LISTEN_RESUME, 1},

    /* === Figure 2-16, NODE_1 (the side that disconnects first) ============ */

    /* "o SYSAP INVOKES DISCONNECT / o SCS SENDS 'DISCONNECT_REQ /
     *  o CONN STATE = 'DISC SENT'" (Fig 2-16). p. 2-26: "the connection has
     *  transitioned from the OPEN state to the DISCONNECT SENT state." */
    {SCS_CONN_OPEN, SCS_CONN_EV_SVC_DISCONNECT,
     SCS_CONN_DISC_SENT, SCS_CONN_ACT_SEND_DISCONNECT_REQ, 0, 1},

    /* p. 2-26: "NODE_2's SCS code sends a disconnect response (DISCONNECT_RSP)
     * to NODE_1's SCS code. This causes the connection to enter the DISCONNECT
     * ACKNOWLEDGED state on NODE_1." */
    {SCS_CONN_DISC_SENT, SCS_CONN_EV_RCV_DISCONNECT_RSP,
     SCS_CONN_DISC_ACK, SCS_CONN_ACT_NONE, 0, 1},

    /* p. 2-26: "When the SCS code on NODE_1 receives the matching
     * DISCONNECT_REQ, it sends a matching DISCONNECT_RSP to NODE_2. From
     * NODE_1's point of view, the connection is now CLOSED." */
    {SCS_CONN_DISC_ACK, SCS_CONN_EV_RCV_DISCONNECT_REQ,
     SCS_CONN_CLOSED, SCS_CONN_ACT_SEND_DISCONNECT_RSP, 0, 1},

    /* p. 2-27, the SIMULTANEOUS-DISCONNECT case: "When each node receives the
     * DISCONNECT_REQ from the other node, it replies with a DISCONNECT_RSP. It
     * also changes the state of the connection relative to itself directly to
     * DISCONNECT MATCH since it has seen a matching DISCONNECT_REQ." This is
     * the row that makes the three-state symmetric path work; without it a
     * simultaneous disconnect scores an illegal event on both nodes. */
    {SCS_CONN_DISC_SENT, SCS_CONN_EV_RCV_DISCONNECT_REQ,
     SCS_CONN_DISC_MATCH, SCS_CONN_ACT_SEND_DISCONNECT_RSP, 0, 1},

    /* === Figure 2-16, NODE_2 (the side that answers) ====================== */

    /* p. 2-26: "NODE_2's SCS code also notifies the SYSAP on NODE_2 that the
     * DISCONNECT_REQ was received. In so doing, it changes the state of the
     * connection from NODE_2's point of view from OPEN to DISCONNECT RECEIVED."
     * The DISCONNECT_RSP goes out on this same step (Fig 2-16). */
    {SCS_CONN_OPEN, SCS_CONN_EV_RCV_DISCONNECT_REQ,
     SCS_CONN_DISC_RECEIVED, SCS_CONN_ACT_SEND_DISCONNECT_RSP,
     SCS_CONN_NOTIFY_DISCONNECTED, 1},

    /* p. 2-26: "The SYSAP on NODE_2 symmetrically invokes the DISCONNECT
     * service, thus causing the SCS code on NODE_2 to send a matching
     * DISCONNECT_REQ to NODE_1. SCS code in NODE_2 then changes the state of
     * the connection from DISCONNECT RECEIVED to DISCONNECT MATCH." */
    {SCS_CONN_DISC_RECEIVED, SCS_CONN_EV_SVC_DISCONNECT,
     SCS_CONN_DISC_MATCH, SCS_CONN_ACT_SEND_DISCONNECT_REQ, 0, 1},

    /* p. 2-26: "When NODE_2's SCS code receives the matching DISCONNECT_RSP
     * from NODE_1, it changes the state of the connection from DISCONNECT MATCH
     * to CLOSED." */
    {SCS_CONN_DISC_MATCH, SCS_CONN_EV_RCV_DISCONNECT_RSP,
     SCS_CONN_CLOSED, SCS_CONN_ACT_NONE, 0, 1},

    /* === OVMX ADDITIONS (doc == 0) -- retransmit tolerance ================
     *
     * NOT VMS-AUTHENTIC and not presented as such (rule 8). SCA does not define
     * what a node does when a formation message it has already answered arrives
     * again. Our own lab wire does: the established VAX retransmits its
     * VMS$VAXcluster CONNECT-REQUEST until it accepts the answer, and scsd.c has
     * deliberately re-answered every one since vms-c6d ("RE-ANSWER each request
     * ... instead of going silent once bound -- a lost response then
     * self-heals"). Without these three rows every such retransmit would be
     * scored an illegal event and the log would be unreadable. They repeat the
     * action of the row they shadow and do NOT move the state. */

    {SCS_CONN_CONNECT_SENT, SCS_CONN_EV_SVC_CONNECT,
     SCS_CONN_CONNECT_SENT, SCS_CONN_ACT_SEND_CONNECT_REQ, 0, 0},

    {SCS_CONN_CONNECT_REC, SCS_CONN_EV_RCV_CONNECT_REQ,
     SCS_CONN_CONNECT_REC, SCS_CONN_ACT_SEND_CONNECT_RSP, 0, 0},

    {SCS_CONN_ACCEPT_SENT, SCS_CONN_EV_RCV_CONNECT_REQ,
     SCS_CONN_ACCEPT_SENT, SCS_CONN_ACT_SEND_ACCEPT_REQ, 0, 0},

    /* === OVMX ADDITION (doc == 0) -- a LOST or unclassified CONNECT_RSP =====
     *
     * NOT VMS-AUTHENTIC. Figure 2-14 requires ACCEPT_REQ to arrive in CONNECT
     * ACK, i.e. after a CONNECT_RSP. The CONNECT_RSP for the VMS$VAXcluster
     * connect DOES exist on our wire -- docs/cluster-protocol-spec.md sec
     * 4(h)(1a) grounds it as the 66-byte [46:48]==1 frame, 16 of 16 dialogues --
     * and scsd.c now classifies it, so the documented path is the normal one.
     *
     * This row exists for the case where that frame is lost or is not
     * recognized: without it a connection the peer demonstrably accepted (it
     * answered with both Con.IDs bound) would park forever in CONNECT SENT and
     * the exit diagnostic would call a working connection stalled. That is a
     * worse lie than a labeled row, and its log line says which row ran. The
     * action it returns (send ACCEPT_RSP) is one OVMX has no builder for;
     * scsd.c LOGS the unemitted action rather than pretending it went out. */
    {SCS_CONN_CONNECT_SENT, SCS_CONN_EV_RCV_ACCEPT_REQ,
     SCS_CONN_OPEN, SCS_CONN_ACT_SEND_ACCEPT_RSP, SCS_CONN_NOTIFY_ACCEPTED, 0},

    /* === OVMX ADDITIONS (doc == 0) -- VIRTUAL CIRCUIT LOSS (vms-abc) ========
     *
     * p. 2-31: "if either the guarantee of message delivery or the guarantee of
     * message sequentiality cannot be satisfied, the virtual circuit between the
     * ports involved will be explicitly broken (if it isn't already). If this
     * happens, then every connection supported by this virtual circuit is also
     * broken, and the SYSAPs participating in these connections are notified of
     * the event."
     *
     * WHY doc == 0 EVEN THOUGH THE BOOK SAYS IT. The two things the rows encode
     * are exactly the two things p. 2-31 states -- the connection is BROKEN (so
     * the state is CLOSED, the state Figure 2-14 gives a connection that is not
     * formed) and the SYSAP is NOTIFIED (so the notify bit is
     * SCS_CONN_NOTIFY_DISCONNECTED, the same bit Figure 2-16 raises when SCS
     * notifies a SYSAP that its connection went away). What is NOT in the book
     * is an ARROW: no figure draws a VC-loss transition, and nothing in ch. 2
     * says a VC loss is processed by the connection state machine as an event.
     * Modelling it that way is an OVMX choice and is flagged as one, so a run
     * log can never present these lines as figure transitions.
     *
     * ONE ROW PER STATE, no wildcards: the table is a list of (from, event)
     * pairs and scs_conn_table_lookup() is a linear scan of it, so a wildcard
     * would be a second mechanism. Written out, every row is independently
     * walkable by a test.
     *
     * NO ACTION on any row. The circuit is gone; p. 2-31's whole point is that
     * a message cannot be sent in its absence ("Any attempt to send a message
     * from one port to another in the absence of a virtual circuit will fail").
     * A row that asked for a DISCONNECT_REQ here would be asking OVMX to
     * transmit into a circuit it has just declared broken. */

    /* A CDT still queued to the Path Block but never formed. Nothing was
     * broken, so nothing is notified -- but the row EXISTS so that a VC loss
     * over a circuit carrying an unformed connection is not scored as an
     * illegal event. */
    {SCS_CONN_CLOSED, SCS_CONN_EV_VC_LOST,
     SCS_CONN_CLOSED, SCS_CONN_ACT_NONE, 0, 0},

    {SCS_CONN_CONNECT_SENT, SCS_CONN_EV_VC_LOST,
     SCS_CONN_CLOSED, SCS_CONN_ACT_NONE, SCS_CONN_NOTIFY_DISCONNECTED, 0},
    {SCS_CONN_CONNECT_ACK, SCS_CONN_EV_VC_LOST,
     SCS_CONN_CLOSED, SCS_CONN_ACT_NONE, SCS_CONN_NOTIFY_DISCONNECTED, 0},
    {SCS_CONN_CONNECT_REC, SCS_CONN_EV_VC_LOST,
     SCS_CONN_CLOSED, SCS_CONN_ACT_NONE, SCS_CONN_NOTIFY_DISCONNECTED, 0},
    {SCS_CONN_ACCEPT_SENT, SCS_CONN_EV_VC_LOST,
     SCS_CONN_CLOSED, SCS_CONN_ACT_NONE, SCS_CONN_NOTIFY_DISCONNECTED, 0},
    {SCS_CONN_REJECT_SENT, SCS_CONN_EV_VC_LOST,
     SCS_CONN_CLOSED, SCS_CONN_ACT_NONE, SCS_CONN_NOTIFY_DISCONNECTED, 0},
    {SCS_CONN_OPEN, SCS_CONN_EV_VC_LOST,
     SCS_CONN_CLOSED, SCS_CONN_ACT_NONE, SCS_CONN_NOTIFY_DISCONNECTED, 0},
    {SCS_CONN_DISC_SENT, SCS_CONN_EV_VC_LOST,
     SCS_CONN_CLOSED, SCS_CONN_ACT_NONE, SCS_CONN_NOTIFY_DISCONNECTED, 0},
    {SCS_CONN_DISC_ACK, SCS_CONN_EV_VC_LOST,
     SCS_CONN_CLOSED, SCS_CONN_ACT_NONE, SCS_CONN_NOTIFY_DISCONNECTED, 0},
    {SCS_CONN_DISC_RECEIVED, SCS_CONN_EV_VC_LOST,
     SCS_CONN_CLOSED, SCS_CONN_ACT_NONE, SCS_CONN_NOTIFY_DISCONNECTED, 0},
    {SCS_CONN_DISC_MATCH, SCS_CONN_EV_VC_LOST,
     SCS_CONN_CLOSED, SCS_CONN_ACT_NONE, SCS_CONN_NOTIFY_DISCONNECTED, 0}
};

#define CONN_TABLE_ROWS ((unsigned)(sizeof(conn_table) / sizeof(conn_table[0])))

/* --------------------------------------------------------------------- */

static void fill_illegal(enum scs_conn_state from, struct scs_conn_transition *out)
{
    memset(out, 0, sizeof(*out));
    out->from = from;
    out->to = from;
    out->action = SCS_CONN_ACT_NONE;
    out->notify = 0;
    out->documented = 0;
    out->illegal = 1;
    out->suppressed = 0;
}

static void fill_from_rule(const struct conn_rule *r, struct scs_conn_transition *out)
{
    memset(out, 0, sizeof(*out));
    out->from = r->from;
    out->to = r->to;
    out->action = r->action;
    out->notify = r->notify;
    out->documented = r->doc;
    out->illegal = 0;
    out->suppressed = 0;
}

int scs_conn_table_lookup(enum scs_conn_state from, enum scs_conn_event ev,
                          struct scs_conn_transition *out)
{
    if (out == NULL) {
        return 0;
    }
    for (unsigned i = 0; i < CONN_TABLE_ROWS; i++) {
        if (conn_table[i].from == from && conn_table[i].ev == ev) {
            fill_from_rule(&conn_table[i], out);
            return 1;
        }
    }
    fill_illegal(from, out);
    return 0;
}

unsigned scs_conn_table_size(void)
{
    return CONN_TABLE_ROWS;
}

int scs_conn_table_row(unsigned i, enum scs_conn_event *ev_out,
                       struct scs_conn_transition *out)
{
    if (out == NULL || i >= CONN_TABLE_ROWS) {
        return 0;
    }
    fill_from_rule(&conn_table[i], out);
    if (ev_out != NULL) {
        *ev_out = conn_table[i].ev;
    }
    return 1;
}

int scs_conn_event_for_msgtype(unsigned msgtype, enum scs_conn_event *ev)
{
    /* See scs_conn.h for the grounding of each value and for which of them have
     * never been seen on a wire. */
    if (ev == NULL) {
        return 0;
    }
    switch (msgtype) {
    case 0: *ev = SCS_CONN_EV_RCV_CONNECT_REQ; return 1;
    case 1: *ev = SCS_CONN_EV_RCV_CONNECT_RSP; return 1;
    case 2: *ev = SCS_CONN_EV_RCV_ACCEPT_REQ; return 1;
    case 3: *ev = SCS_CONN_EV_RCV_ACCEPT_RSP; return 1;
    case 4: *ev = SCS_CONN_EV_RCV_REJECT_REQ; return 1;
    case 5: *ev = SCS_CONN_EV_RCV_REJECT_RSP; return 1;     /* never observed */
    case 6: *ev = SCS_CONN_EV_RCV_DISCONNECT_REQ; return 1;
    case 7: *ev = SCS_CONN_EV_RCV_DISCONNECT_RSP; return 1; /* never observed */
    default: return 0;
    }
}

int scs_conn_fsm_enabled(void)
{
    /* Read fresh every call (see scs_conn.h): a cached answer could not be
     * bracketed by a test, and guardrail 23 requires the switch be RUN. */
    const char *v = getenv("OVMX_NO_CONN_FSM");
    if (v == NULL || v[0] == '\0') {
        return 1;
    }
    if (v[0] == '0' && v[1] == '\0') {
        return 1;
    }
    return 0;
}

void scs_conn_fsm_init(struct scs_cdt *cdt)
{
    if (cdt == NULL) {
        return;
    }
    cdt->conn_state = (int)SCS_CONN_CLOSED;
}

enum scs_conn_state scs_conn_state_of(const struct scs_cdt *cdt)
{
    if (cdt == NULL) {
        return SCS_CONN_CLOSED;
    }
    if (cdt->conn_state < 0 || cdt->conn_state >= SCS_CONN_STATE_COUNT) {
        return SCS_CONN_CLOSED;
    }
    return (enum scs_conn_state)cdt->conn_state;
}

/* --- logging ------------------------------------------------------------- */

static FILE *conn_log_fp = NULL;

void scs_conn_set_log(FILE *fp)
{
    conn_log_fp = fp;
}

/* Human-readable notify bits, appended to the transition line. Never empty. */
static void notify_str(unsigned notify, char *buf, size_t cap)
{
    static const struct {
        unsigned    bit;
        const char *name;
    } bits[] = {
        {SCS_CONN_NOTIFY_CONNECT_REQ, "pass-CONNECT_REQ-to-SYSAP"},
        {SCS_CONN_NOTIFY_ACCEPTED, "SYSAP-accepted"},
        {SCS_CONN_NOTIFY_REJECTED, "SYSAP-rejected"},
        {SCS_CONN_NOTIFY_DISCONNECTED, "SYSAP-disconnected"},
        {SCS_CONN_NOTIFY_LISTEN_STOP, "stop-listening"},
        {SCS_CONN_NOTIFY_LISTEN_RESUME, "resume-listening"}
    };
    size_t used = 0;
    buf[0] = '\0';
    for (unsigned i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        if ((notify & bits[i].bit) == 0) {
            continue;
        }
        size_t n = strlen(bits[i].name);
        if (used + n + 2 >= cap) {
            break;
        }
        if (used > 0) {
            buf[used++] = ',';
        }
        memcpy(buf + used, bits[i].name, n);
        used += n;
        buf[used] = '\0';
    }
    if (used == 0 && cap > 1) {
        buf[0] = '-';
        buf[1] = '\0';
    }
}

/*
 * Every transition is logged WITH THE CONID (the done condition: "a run log
 * shows the state history of every connection"). The remote CONID is included
 * because a CDT is one half of a mirror-image pair (p. 2-28) and a log with
 * only the local half cannot be correlated against a capture.
 */
static void log_transition(const struct scs_cdt *cdt, enum scs_conn_event ev,
                           const struct scs_conn_transition *t)
{
    if (conn_log_fp == NULL) {
        return;
    }
    char nbuf[160];
    notify_str(t->notify, nbuf, sizeof(nbuf));
    fprintf(conn_log_fp,
            "SCSD-I-CONNFSM, conid=0x%08X remote=0x%08X %s->%s: %s --%s--> %s"
            " action=%s notify=%s%s%s\n",
            (unsigned)cdt->local_conid, (unsigned)cdt->remote_conid,
            cdt->local_sysap[0] ? cdt->local_sysap : "?",
            cdt->remote_sysap[0] ? cdt->remote_sysap : "?",
            scs_conn_state_name(t->from), scs_conn_event_name(ev),
            scs_conn_state_name(t->to), scs_conn_action_name(t->action), nbuf,
            t->illegal ? " ILLEGAL-EVENT-FOR-THIS-STATE" : "",
            (!t->illegal && !t->documented) ? " [OVMX row, not in Fig 2-14/15/16]" : "");
    fflush(conn_log_fp);
}

struct scs_conn_transition scs_conn_fsm_step(struct scs_cdt *cdt, enum scs_conn_event ev)
{
    struct scs_conn_transition t;

    if (cdt == NULL) {
        fill_illegal(SCS_CONN_CLOSED, &t);
        return t;
    }
    if (!scs_conn_fsm_enabled()) {
        /* The kill switch. Nothing is stored, nothing is logged; the caller can
         * see from `suppressed` that no state was tracked (as opposed to a
         * state that legitimately did not move). */
        memset(&t, 0, sizeof(t));
        t.from = scs_conn_state_of(cdt);
        t.to = t.from;
        t.action = SCS_CONN_ACT_NONE;
        t.suppressed = 1;
        return t;
    }

    enum scs_conn_state from = scs_conn_state_of(cdt);
    (void)scs_conn_table_lookup(from, ev, &t);
    if (!t.illegal) {
        cdt->conn_state = (int)t.to;
    }
    log_transition(cdt, ev, &t);
    return t;
}

unsigned scs_conn_report_stuck(const struct scs_cdl *cdl, FILE *fp)
{
    if (!scs_conn_fsm_enabled()) {
        /* Never report "0 stuck" when the truth is "nothing was tracked". */
        if (fp != NULL) {
            fprintf(fp, "SCSD-I-CONNSTUCK, connection state machine DISABLED"
                        " (OVMX_NO_CONN_FSM) -- no connection states were tracked,"
                        " so none can be reported\n");
            fflush(fp);
        }
        return 0;
    }
    if (cdl == NULL) {
        return 0;
    }

    unsigned stuck = 0;
    unsigned connections = 0;
    for (unsigned i = 0; i < SCS_CDL_ENTRIES; i++) {
        const struct scs_cdt *cdt = cdl->entry[i];
        if (cdt == NULL || !cdt->in_use) {
            continue;
        }
        /* vms-7fe: a p. 2-48 LISTENING CDT describes no connection -- it has no
         * peer, no circuit and no Figure 2-14 state -- so it is neither stuck
         * nor counted. Reporting one as "parked off OPEN" would put a permanent
         * false warning in every run's exit summary. */
        if (cdt->listening) {
            continue;
        }
        connections++;
        enum scs_conn_state st = scs_conn_state_of(cdt);
        if (st == SCS_CONN_OPEN) {
            continue;
        }
        stuck++;
        if (fp != NULL) {
            fprintf(fp,
                    "SCSD-W-CONNSTUCK, conid=0x%08X remote=0x%08X %s->%s parked in %s"
                    " (never reached OPEN, or left it and never closed)\n",
                    (unsigned)cdt->local_conid, (unsigned)cdt->remote_conid,
                    cdt->local_sysap[0] ? cdt->local_sysap : "?",
                    cdt->remote_sysap[0] ? cdt->remote_sysap : "?",
                    scs_conn_state_name(st));
        }
    }
    if (fp != NULL) {
        fprintf(fp, "SCSD-I-CONNSTUCK, %u of %u in-use connection(s) parked off OPEN\n",
                stuck, connections);
        fflush(fp);
    }
    return stuck;
}

/* --- names --------------------------------------------------------------- */

const char *scs_conn_state_name(enum scs_conn_state st)
{
    /* The figures' own spellings, so a log line can be diffed against the book. */
    switch (st) {
    case SCS_CONN_CLOSED:
        return "CLOSED";
    case SCS_CONN_CONNECT_SENT:
        return "CONNECT SENT";
    case SCS_CONN_CONNECT_ACK:
        return "CONNECT ACK";
    case SCS_CONN_CONNECT_REC:
        return "CONNECT REC";
    case SCS_CONN_ACCEPT_SENT:
        return "ACCEPT SENT";
    case SCS_CONN_REJECT_SENT:
        return "REJECT SENT";
    case SCS_CONN_OPEN:
        return "OPEN";
    case SCS_CONN_DISC_SENT:
        return "DISC SENT";
    case SCS_CONN_DISC_ACK:
        return "DISC ACK";
    case SCS_CONN_DISC_RECEIVED:
        return "DISC RECEIVED";
    case SCS_CONN_DISC_MATCH:
        return "DISC MATCH";
    default:
        return "?";
    }
}

const char *scs_conn_event_name(enum scs_conn_event ev)
{
    switch (ev) {
    case SCS_CONN_EV_SVC_CONNECT:
        return "SVC_CONNECT";
    case SCS_CONN_EV_SVC_ACCEPT:
        return "SVC_ACCEPT";
    case SCS_CONN_EV_SVC_REJECT:
        return "SVC_REJECT";
    case SCS_CONN_EV_SVC_DISCONNECT:
        return "SVC_DISCONNECT";
    case SCS_CONN_EV_RCV_CONNECT_REQ:
        return "RCV_CONNECT_REQ";
    case SCS_CONN_EV_RCV_CONNECT_RSP:
        return "RCV_CONNECT_RSP";
    case SCS_CONN_EV_RCV_ACCEPT_REQ:
        return "RCV_ACCEPT_REQ";
    case SCS_CONN_EV_RCV_ACCEPT_RSP:
        return "RCV_ACCEPT_RSP";
    case SCS_CONN_EV_RCV_REJECT_REQ:
        return "RCV_REJECT_REQ";
    case SCS_CONN_EV_RCV_REJECT_RSP:
        return "RCV_REJECT_RSP";
    case SCS_CONN_EV_RCV_DISCONNECT_REQ:
        return "RCV_DISCONNECT_REQ";
    case SCS_CONN_EV_RCV_DISCONNECT_RSP:
        return "RCV_DISCONNECT_RSP";
    case SCS_CONN_EV_VC_LOST:
        return "VC_LOST";
    default:
        return "?";
    }
}

const char *scs_conn_action_name(enum scs_conn_action act)
{
    switch (act) {
    case SCS_CONN_ACT_NONE:
        return "none";
    case SCS_CONN_ACT_SEND_CONNECT_REQ:
        return "send CONNECT_REQ";
    case SCS_CONN_ACT_SEND_CONNECT_RSP:
        return "send CONNECT_RSP";
    case SCS_CONN_ACT_SEND_ACCEPT_REQ:
        return "send ACCEPT_REQ";
    case SCS_CONN_ACT_SEND_ACCEPT_RSP:
        return "send ACCEPT_RSP";
    case SCS_CONN_ACT_SEND_REJECT_REQ:
        return "send REJECT_REQ";
    case SCS_CONN_ACT_SEND_REJECT_RSP:
        return "send REJECT_RSP";
    case SCS_CONN_ACT_SEND_DISCONNECT_REQ:
        return "send DISCONNECT_REQ";
    case SCS_CONN_ACT_SEND_DISCONNECT_RSP:
        return "send DISCONNECT_RSP";
    default:
        return "?";
    }
}
