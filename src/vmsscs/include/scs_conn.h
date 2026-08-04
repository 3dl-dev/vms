/*
 * scs_conn.h - THE SCA CONNECTION STATE MACHINE (vms-dd5).
 *
 * WHY THIS EXISTS. vms-e1a gave OVMX a connection OBJECT (struct scs_cdt) with
 * a field called `conn_state` that nothing ever wrote, and scs_cdt.h says so in
 * as many words: "`conn_state` is an OPAQUE int here. This module stores it and
 * never interprets it. The state values and every transition between them
 * belong to the SCS connection state machine (vms-dd5)." This is that machine.
 *
 * Before it, a connection in OVMX was three booleans in scsd.c's struct
 * peer_state -- `connected`, `dir_connected`, `joiner_connected` -- each set
 * once and never cleared. A connection that stalled part-way through formation
 * was indistinguishable from one that had never been attempted: the boolean was
 * 0 in both cases, no state was recorded, nothing was logged, and no diagnostic
 * could say which. Measured over src/vmsscs/ before this item, `conn_state`,
 * `ACCEPT_REQ`, `REJECT_REQ` and `DISCONNECT_REQ` appeared ZERO times outside
 * scs_cdt.h's own commentary.
 *
 * SOURCE (public, quoted with page cites -- CLAUDE.md rule 8):
 *   Roy G. Davis, *VAXcluster Principles*, Digital Press 1993, ch. 2.
 *   - Figure 2-14 "SCA Connection Formation" + its narrative       pp. 2-23/2-24
 *   - Figure 2-15 "SCA Connection Rejection" + its narrative        p. 2-24/2-25
 *   - the 16-bit reason code in REJECT_REQ / DISCONNECT_REQ         p. 2-26
 *   - Figure 2-16 "Explicitly Breaking an SCA Connection" + narrative
 *                                                                  pp. 2-26/2-27
 *   - the simultaneous-DISCONNECT case ("both sides ... transitioning through
 *     only three states")                                           p. 2-27
 *
 * THIS MODULE IS PURE. It builds no frame, parses no frame and opens no socket.
 * scs_conn_table_lookup() is a total function of (state, event) over a static
 * const table and touches no memory at all; scs_conn_fsm_step() is that lookup
 * plus one store into cdt->conn_state plus an optional log line. Every path is
 * reachable from a unit test with no socket -- see tests/vmsscs/test_scs_conn.c.
 *
 * ======================= WIRE VERDICT (read this) =========================
 * vms-dd5 was dispatched wire-visible YES. IT SHIPPED WIRE-INVISIBLE. Not one
 * byte of any frame OVMX emits is a function of any value defined in this
 * header: the machine RECORDS the state a connection is in and LOGS every
 * transition, but scsd.c calls the same builders with the same arguments in the
 * same order as before, and it keeps the three Con.IDs OVMX has been putting on
 * the lab wire since vms-5fe by claiming their CDL slots explicitly
 * (scs_cdl_alloc_conid) rather than taking whatever the allocator hands out.
 * tests/vmsscs/test_scsd_wire.c::test_conn_fsm_does_not_change_the_wire()
 * asserts the emitted CONNECT-REQUEST frame is BYTE-IDENTICAL with the machine
 * enabled and with OVMX_NO_CONN_FSM=1, over the real scsd.c translation unit.
 *
 * WHAT THE KILL SWITCH ACTUALLY GATES, stated exactly (guardrail 23): setting
 * OVMX_NO_CONN_FSM=1 suppresses (a) CDT allocation in scsd.c, (b) every state
 * transition, (c) every SCSD-I-CONNFSM transition log line, and (d) the
 * end-of-run SCSD-W-CONNSTUCK diagnostic. It gates NO emitted byte, because the
 * machine emits none. Claiming otherwise would be the "kill-switch that did not
 * gate it" defect this epic has already rejected once.
 *
 * WHAT WAS NOT DONE, stated so nobody has to infer it: the switch was NOT run
 * on a lab pod, and no bracketed lab triple was performed. It was run over the
 * production src/vmsscs/scsd.c translation unit through the SCSD_UNIT_TEST
 * transmit seam, which exercises the real conn_bind / conn_step / send path,
 * and the transition counter was confirmed to move with the switch off before
 * anything was claimed about it being on. Confirming the run-log behaviour
 * end to end needs CAP_NET_RAW and a peer; that is `tests/lab/tools/lab2run.sh`
 * with and without OVMX_NO_CONN_FSM=1, and it has not been done.
 * ==========================================================================
 *
 * ================= WHAT IS DOCUMENTED VS WHAT IS OVMX =====================
 * Every row of the transition table carries a `documented` flag.
 *
 *   documented == 1  -- the transition is drawn in Figure 2-14, 2-15 or 2-16 or
 *                       stated in the p. 2-23/2-26/2-27 narrative. There are 16
 *                       such rows and they are the WHOLE of the three figures;
 *                       test_scs_conn.c::test_every_figure_transition_is_present
 *                       walks the figures independently and asserts each one.
 *
 *   documented == 0  -- an OVMX ADDITION, labeled per rule 8. There are 4, each
 *                       carrying its own justification in scs_conn.c:
 *                       - three RETRANSMIT-TOLERANCE rows. SCA does not say what
 *                         a node does when a formation message it has already
 *                         answered arrives again, but the real VAX retransmits
 *                         its CONNECT-REQUEST until it likes the answer (scsd.c
 *                         has re-answered them since vms-c6d), so without these
 *                         every retransmit would score as an illegal event. They
 *                         repeat the original action and do NOT move the state.
 *                       - one row tolerating a LOST or unclassified CONNECT_RSP,
 *                         so that ACCEPT_REQ arriving in CONNECT SENT rather
 *                         than CONNECT ACK does not park a working connection.
 *                       None of the four is presented as VMS-authentic.
 * ==========================================================================
 *
 * ============ THE ROLE MAPPING IS THE DAEMON'S, NOT THIS MODULE'S ==========
 * This header defines the machine. WHICH observed frame counts as which SCA
 * message lives at the call sites in src/vmsscs/scsd.c, next to the frame that
 * triggers it. That mapping was the weakest part of this item when it started
 * and it is now the best-grounded: vms-dd5 measured the connection-control
 * frames of formation-ci1.pcap and found that the [46:48] field this project
 * had been calling "a per-dialogue message counter" is the SCA
 * CONNECTION-CONTROL MESSAGE TYPE, and that the four messages of Figure 2-14
 * are literally on the wire, in order, for five different SYSAPs:
 *
 *     0 = CONNECT_REQ   1 = CONNECT_RSP   2 = ACCEPT_REQ   3 = ACCEPT_RSP
 *
 * 16 complete dialogues, 16 CONNECT_REQ / 16 CONNECT_RSP, each terminated by
 * exactly one of 6 ACCEPT_REQ or 10 value-`4` frames with zero overlap. The
 * derivation, the counts, the caveats on `4` (REJECT_REQ) and `6` (DISCONNECT),
 * and the fact that `5` and `7` appear on NO capture we hold, are all in
 * docs/cluster-protocol-spec.md sec 4(h)(1a) -- which vms-dd5 also had to
 * CORRECT, since the counter reading is refuted. Read that section before
 * changing any mapping here.
 *
 * OVMX does not yet build a REJECT_REQ, a DISCONNECT_REQ, an ACCEPT_RSP or a
 * CONNECT_RSP of its own; where the machine says to send one, scsd.c logs
 * SCSD-W-CONNNOACT rather than pretending a frame went out.
 * ==========================================================================
 *
 * WHAT THIS ITEM DOES NOT DO. It does not fix the vms-2f3 rejoin failure and
 * nothing here should be read as claiming it does. It gives that investigation
 * something it did not have: a connection whose state is a named value, whose
 * every transition is on stdout next to its CONID, and which is REPORTED at
 * process exit if it is parked anywhere other than OPEN -- which is precisely
 * the shape of the peer's behaviour recorded in docs/HANDOFF-vms-2f3.md
 * sec 4M.18/4M.28 (a CDT parked in disc_sent/disc_pend).
 */
#ifndef SCS_CONN_H
#define SCS_CONN_H

#include <stdio.h>

#include "scs_cdt.h" /* struct scs_cdt (conn_state lives there), struct scs_cdl */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * enum scs_conn_state - "State of the connection." (p. 2-28, the CDT field this
 * enumerates). Every state named in Figures 2-14, 2-15 and 2-16, using the
 * figures' own abbreviated spellings.
 *
 * CLOSED is 0 so that a zeroed/never-touched struct scs_cdt reads as CLOSED,
 * which is what Figure 2-14 calls the state a connection is in before its SYSAP
 * invokes CONNECT ("o CONN STATE = 'CLOSED'").
 */
enum scs_conn_state {
    SCS_CONN_CLOSED = 0,        /* Fig 2-14/2-15 NODE_1 start; Fig 2-16 both ends finish here */
    SCS_CONN_CONNECT_SENT = 1,  /* Fig 2-14 "CONNECT SENT"   -- source sent CONNECT_REQ */
    SCS_CONN_CONNECT_ACK = 2,   /* Fig 2-14 "CONNECT ACK"    -- source got CONNECT_RSP */
    SCS_CONN_CONNECT_REC = 3,   /* Fig 2-14 "CONNECT REC"    -- target passed req to its SYSAP */
    SCS_CONN_ACCEPT_SENT = 4,   /* Fig 2-14 "ACCEPT SENT"    -- target sent ACCEPT_REQ */
    SCS_CONN_REJECT_SENT = 5,   /* Fig 2-15 "REJECT SENT"    -- target sent REJECT_REQ */
    SCS_CONN_OPEN = 6,          /* Fig 2-14 "OPEN" */
    SCS_CONN_DISC_SENT = 7,     /* Fig 2-16 "DISC SENT" */
    SCS_CONN_DISC_ACK = 8,      /* Fig 2-16 "DISC ACK" */
    SCS_CONN_DISC_RECEIVED = 9, /* Fig 2-16 "DISC RECEIVED" */
    SCS_CONN_DISC_MATCH = 10    /* Fig 2-16 "DISC MATCH" */
};
#define SCS_CONN_STATE_COUNT 11

/*
 * enum scs_conn_event - everything that can drive the machine.
 *
 * Two kinds, exactly as the figures draw them. The left-hand bullets of each
 * NODE column are LOCAL SYSAP SERVICE INVOCATIONS ("SOURCE SYSAP INVOKES
 * CONNECT", "TARGET SYSAP INVOKES ACCEPT/REJECT", "SYSAP INVOKES DISCONNECT");
 * the labelled arrows between the columns are RECEIVED SCS CONTROL MESSAGES.
 */
enum scs_conn_event {
    /* Local SYSAP service invocations. */
    SCS_CONN_EV_SVC_CONNECT = 0,    /* the CONNECT service    (p. 2-22) */
    SCS_CONN_EV_SVC_ACCEPT = 1,     /* the ACCEPT service     (p. 2-23) */
    SCS_CONN_EV_SVC_REJECT = 2,     /* the REJECT service     (p. 2-24) */
    SCS_CONN_EV_SVC_DISCONNECT = 3, /* the DISCONNECT service (p. 2-26) */
    /* Received SCS control messages. */
    SCS_CONN_EV_RCV_CONNECT_REQ = 4,
    SCS_CONN_EV_RCV_CONNECT_RSP = 5,
    SCS_CONN_EV_RCV_ACCEPT_REQ = 6,
    SCS_CONN_EV_RCV_ACCEPT_RSP = 7,
    SCS_CONN_EV_RCV_REJECT_REQ = 8,
    SCS_CONN_EV_RCV_REJECT_RSP = 9,
    SCS_CONN_EV_RCV_DISCONNECT_REQ = 10,
    SCS_CONN_EV_RCV_DISCONNECT_RSP = 11
};
#define SCS_CONN_EVENT_COUNT 12

/*
 * enum scs_conn_action - the packet SCS must send as a result of the
 * transition, or NONE. One action per transition; the figures never draw two
 * sends on one arrow.
 */
enum scs_conn_action {
    SCS_CONN_ACT_NONE = 0,
    SCS_CONN_ACT_SEND_CONNECT_REQ = 1,
    SCS_CONN_ACT_SEND_CONNECT_RSP = 2,
    SCS_CONN_ACT_SEND_ACCEPT_REQ = 3,
    SCS_CONN_ACT_SEND_ACCEPT_RSP = 4,
    SCS_CONN_ACT_SEND_REJECT_REQ = 5,
    SCS_CONN_ACT_SEND_REJECT_RSP = 6,
    SCS_CONN_ACT_SEND_DISCONNECT_REQ = 7,
    SCS_CONN_ACT_SEND_DISCONNECT_RSP = 8
};

/*
 * SYSAP notifications the figures draw as their own bullets. Returned as a
 * bitmask so a transition can both send a packet and notify (Figure 2-14's
 * "o SCS SENDS 'ACCEPT_RSP'" + "o SCS NOTIFIES SOURCE SYSAP ABOUT ACCEPTANCE").
 * OVMX has no SYSAP callback wiring for these yet -- scsd.c logs them; the
 * scs_cdt.h msg_input/dgram_input/vc_loss handlers are the eventual home.
 */
#define SCS_CONN_NOTIFY_CONNECT_REQ   0x01u /* "SCS PASSES 'CONNECT_REQ' TO TARGET SYSAP" */
#define SCS_CONN_NOTIFY_ACCEPTED      0x02u /* "SCS NOTIFIES SOURCE SYSAP ABOUT ACCEPTANCE" */
#define SCS_CONN_NOTIFY_REJECTED      0x04u /* "SCS NOTIFIES SOURCE SYSAP" (Fig 2-15) */
#define SCS_CONN_NOTIFY_DISCONNECTED  0x08u /* "SCS NOTIFIES SYSAP" (Fig 2-16) */
#define SCS_CONN_NOTIFY_LISTEN_STOP   0x10u /* "the target SYSAP temporarily stops listening" p. 2-23 */
#define SCS_CONN_NOTIFY_LISTEN_RESUME 0x20u /* "TARGET SYSAP AGAIN 'LISTENING'" */

/*
 * struct scs_conn_transition - the whole result of one step. Returned by value
 * so the transition function needs no output buffer and no allocation.
 */
struct scs_conn_transition {
    enum scs_conn_state  from;
    enum scs_conn_state  to;      /* == from when the event is illegal or the machine is off */
    enum scs_conn_action action;
    unsigned             notify;  /* SCS_CONN_NOTIFY_* bits */
    int                  documented; /* 1 = drawn in Fig 2-14/2-15/2-16; 0 = OVMX addition */
    int                  illegal;    /* 1 = no rule covers (from, event); state left unchanged */
    int                  suppressed; /* 1 = OVMX_NO_CONN_FSM was set; nothing happened at all */
};

/*
 * scs_conn_table_lookup - THE PURE FUNCTION. Total over (state, event); touches
 * no memory outside `out`. Returns 1 if a rule covers the pair (out is filled
 * with it), 0 if none does (out is filled with the illegal result: to == from,
 * action NONE, notify 0, illegal 1). `out` may not be NULL.
 *
 * This is the interface the sibling items consume: everything the machine knows
 * is reachable without a CDT, without a socket and without an environment.
 */
int scs_conn_table_lookup(enum scs_conn_state from, enum scs_conn_event ev,
                          struct scs_conn_transition *out);

/*
 * scs_conn_table_size / scs_conn_table_row - iterate the table itself, so a
 * test can assert the figures are covered ROW BY ROW instead of trusting that
 * the states it happened to walk were the only ones. row() fills *out with row
 * `i` plus the `from` state and event it is keyed on, and returns 1, or returns
 * 0 if i is out of range.
 */
unsigned scs_conn_table_size(void);
int scs_conn_table_row(unsigned i, enum scs_conn_event *ev_out,
                       struct scs_conn_transition *out);

/*
 * scs_conn_event_for_msgtype - map the SCA connection-control MESSAGE TYPE
 * carried on the wire at payload offset [46:48] onto a received-message event.
 * Pure; no socket, no state, no environment.
 *
 * GROUNDING: docs/cluster-protocol-spec.md sec 4(h)(1a), measured by vms-dd5
 * over formation-ci1.pcap's connection-control length classes (110/66/62) --
 * 60 frames, 16 complete dialogues, 0 residuals:
 *     0 = CONNECT_REQ  (110 bytes, destination Con.ID 0, SYSAP name present)
 *     1 = CONNECT_RSP  ( 66 bytes, destination echoed, source still 0)
 *     2 = ACCEPT_REQ   (110 bytes, both filled, SYSAP name present)
 *     3 = ACCEPT_RSP   ( 62 bytes, both bound, original direction)
 * 16 CONNECT_REQ / 16 CONNECT_RSP, each dialogue closed by exactly one of
 * 6 ACCEPT_REQ or 10 value-4 frames, with no Con.ID receiving both. That same
 * measurement REFUTES this project's earlier reading of the field as a
 * per-dialogue message counter.
 *
 * WEAKER, and labeled as such in the spec: 4 = REJECT_REQ rests on a decisive
 * behavioural partition rather than a decoded field; 6 = DISCONNECT_REQ rests
 * on shape alone. WEAKEST: 5 (REJECT_RSP) and 7 (DISCONNECT_RSP) are mapped BY
 * POSITION ONLY -- neither value occurs on any capture this project holds, so
 * OVMX has never seen one and this mapping is untested against a real frame.
 *
 * Returns 1 and fills *ev for 0..7; returns 0 and leaves *ev alone for anything
 * else, notably 10 (the application/SYSAP class carrying every steady-state
 * frame) which is NOT a connection-control message.
 */
int scs_conn_event_for_msgtype(unsigned msgtype, enum scs_conn_event *ev);

/*
 * scs_conn_fsm_enabled - 0 iff the environment variable OVMX_NO_CONN_FSM is set
 * to anything other than "0". Read fresh on every call (NOT cached), so a test
 * can bracket a single call with setenv/unsetenv and see the behaviour change.
 */
int scs_conn_fsm_enabled(void);

/*
 * scs_conn_fsm_init - put a CDT in the CLOSED state, the state Figure 2-14 says
 * a connection is in before its SYSAP invokes CONNECT. No-op on NULL.
 */
void scs_conn_fsm_init(struct scs_cdt *cdt);

/* Current state of a CDT; CLOSED for NULL. */
enum scs_conn_state scs_conn_state_of(const struct scs_cdt *cdt);

/*
 * scs_conn_fsm_step - apply `ev` to `cdt`, storing the new state into
 * cdt->conn_state and logging the transition with the CDT's CONID (see
 * scs_conn_set_log). Returns the transition.
 *
 * - cdt == NULL          -> {CLOSED, CLOSED, NONE, illegal=1}, nothing logged.
 * - machine disabled     -> suppressed=1, state NOT touched, nothing logged.
 * - illegal (state, ev)  -> illegal=1, state NOT touched, LOGGED (an illegal
 *                           event is exactly the thing this item exists to make
 *                           visible, so it is the one case that must never be
 *                           silent).
 */
struct scs_conn_transition scs_conn_fsm_step(struct scs_cdt *cdt, enum scs_conn_event ev);

/*
 * scs_conn_set_log - where transition lines go. NULL (the default) means no
 * logging at all, which is what keeps the machine usable from a unit test and
 * from any caller that has not opted in. Line format:
 *
 *   SCSD-I-CONNFSM, conid=0x4F580007 remote=0x63050008 SCS$DIRECTORY:
 *       CLOSED --RCV_CONNECT_REQ--> CONNECT REC action=send CONNECT_RSP notify=...
 *
 * The CONID is first because the run log is read by grepping for one.
 */
void scs_conn_set_log(FILE *fp);

/*
 * scs_conn_report_stuck - THE DIAGNOSTIC. "A state that is entered and never
 * left is detectable": scan every in-use CDT in the CDL and report each one
 * whose state is not OPEN, with its CONID, its SYSAP names and its state name.
 * Returns the number reported (0 = every open connection is OPEN).
 *
 * Writes to `fp`; pass NULL to count without printing. If the machine is
 * disabled it prints one line saying so and returns 0 -- it must not report
 * "0 stuck connections" when the truth is "nothing was tracked".
 *
 * This is deliberately called at process exit: a connection parked off-OPEN for
 * the whole run is the failure mode this item was cut to expose.
 */
unsigned scs_conn_report_stuck(const struct scs_cdl *cdl, FILE *fp);

/* Names for logs and tests. Never NULL; "?" for an out-of-range value. */
const char *scs_conn_state_name(enum scs_conn_state st);
const char *scs_conn_event_name(enum scs_conn_event ev);
const char *scs_conn_action_name(enum scs_conn_action act);

#ifdef __cplusplus
}
#endif

#endif /* SCS_CONN_H */
