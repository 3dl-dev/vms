/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cnxtrace_names.h - the SYMBOLIC NAMES SYS$SYSTEM:CNXTRACE.EXE renders the
 * join transition ring's ordinals with (E69).
 *
 * WHY THE NAMES ARE HERE AND NOT IN THE EXECUTIVE. The ring carries ORDINALS
 * across /dev/vms, never strings -- a fixed-width record is what makes the
 * struct an ABI both kmods and this ILP32/LP64 client agree on. The executive
 * has its own renderers (cnxman_diag_*_name(), src/kernel-core/
 * vms_cnxman_diag.c) and this userland image cannot link kernel-core, so the
 * tables are duplicated -- the same "ONE facility source, duplicated
 * declaration" shape the CLUSTER_DIAG_* ioctl row structs already use.
 *
 * WHAT KEEPS THE TWO COPIES HONEST. tests/cluster/host/test_cnxman_diag.c
 * includes BOTH this header and the executive's, and compares them ordinal by
 * ordinal for every value of all five vocabularies. A name added on one side
 * and not the other is a RED HOST TEST, not a transcript that quietly renders
 * "?" on a lab console at 2 a.m.
 *
 * A header of plain `static const char *const` tables, so it can be included by
 * exactly two translation units without a link dependency between them.
 */
#ifndef OVMX_CNXTRACE_NAMES_H
#define OVMX_CNXTRACE_NAMES_H

/*
 * enum cnxman_join_state (src/kernel-core/vms_cnxman_join_fsm.h), rendered by
 * cnxman_join_state_name() in the executive.
 */
static const char *const cnxtrace_state_names[] = {
    "IDLE",         /* 0 CNXMAN_JOIN_IDLE          */
    "DIR ROUND",    /* 1 CNXMAN_JOIN_DIR_ROUND     */
    "MSCP CONNECT", /* 2 CNXMAN_JOIN_MSCP_CONNECT  */
    "VC CONNECT",   /* 3 CNXMAN_JOIN_VC_CONNECT    */
    "ADVERTISE",    /* 4 CNXMAN_JOIN_ADVERTISE     */
    "ADMIT",        /* 5 CNXMAN_JOIN_ADMIT         */
    "BARRIER",      /* 6 CNXMAN_JOIN_BARRIER       */
    "MEMBER",       /* 7 CNXMAN_JOIN_MEMBER        */
    "FAILED"        /* 8 CNXMAN_JOIN_FAILED        */
};

/*
 * enum cnxman_join_failure, rendered by cnxman_join_failure_name(). Reported
 * only in the summary line, never per record.
 */
static const char *const cnxtrace_failure_names[] = {
    "none",                          /* 0 */
    "no member to join through",     /* 1 */
    "connect refused locally",       /* 2 */
    "connect rejected by the peer",  /* 3 */
    "path lost",                     /* 4 */
    "SYSAP not present on the member", /* 5 */
    "message could not be sent",     /* 6 */
    "codec refused to build",        /* 7 */
    "reconnect interval expired",    /* 8 */
    "no member answered the membership request"  /* 9 (E80) */
};

/* enum cnxman_diag_kind */
static const char *const cnxtrace_kind_names[] = {
    "DISPATCH", /* 0 */
    "ARRIVAL",  /* 1 */
    "EMIT"      /* 2 */
};

/* enum cnxman_diag_reason (an ARRIVAL record's `detail`) */
static const char *const cnxtrace_reason_names[] = {
    "-",              /* 0 CNXMAN_DIAG_R_NONE       */
    "dispatched",     /* 1 */
    "unparsed",       /* 2 */
    "not-mine",       /* 3 */
    "peer-ack",       /* 4 */
    "accepted",       /* 5 */
    "refused-no-csb", /* 6 */
    "mscp-rejected",  /* 7 */
    "cm-rejected",    /* 8 */
    "not-our-conid",  /* 9 */
    "cdt-open",       /* 10 */
    "cm-accept",      /* 11 */
    "cdt-closed",     /* 12 */
    /*
     * 13 CNXMAN_DIAG_R_SEND_REFUSED (E70). On this record `rc` is the SCS
     * layer's own `enum scs_err` and `aux` is the PORT's own refusal code,
     * verbatim -- the two facts the EMIT record's many-to-one SS$_ status
     * cannot carry. Both columns already print on every line.
     */
    "send-refused",   /* 13 */
    /*
     * 14-19 (E70): WHICH refusal it was, in words. The executive records one
     * of these immediately after every `send-refused`, so a refused EMIT reads
     * as three consecutive lines -- the named cause, the SCS-level refusal,
     * then the message that did not go. On these `rc` is the PORT's own
     * `enum pe_vc_send_status` (or, on cdt-not-sendable, the connection's live
     * `enum vms_scs_cdt_state`) and `aux` is the one live counter or state
     * behind it; both columns already print on every line.
     */
    "port-nocircuit", /* 14  aux = the circuit's live state, or 0xffffffff
                       *     when the port holds no circuit at all         */
    "port-nocredit",  /* 15  aux = sends this circuit refused for credit   */
    "port-ringfull",  /* 16  aux = sends refused for a full unacked ring   */
    "port-badframe",  /* 17  aux = 0: the port keeps no such count         */
    "port-txfail",    /* 18  aux = 0: the interface refused the frame      */
    "cdt-not-sendable",/* 19 rc = the CDT's live state, aux = its Send Credit */
    /*
     * 20 (E71): SCS refused to OPEN a connection. rc = the executive's SS$_
     * status for that connect, aux = the destination SCSSYSTEMID (low 32
     * bits). No companion record names the refusing layer -- a refused
     * connect leaves no CDT to ask, and the port's last refusal may belong to
     * another frame entirely.
     */
    "connect-refused", /* 20 */
    /*
     * 21 (E78): the PEER has spent every receive buffer this node extended on
     * a VMS$VAXcluster connection, so p. 2-43's account leaves it unable to
     * transmit until we return one. rc = this end's Pending Receive Credit at
     * that instant, aux = the Con.ID. Recorded once per connection manager. It
     * is the one line that names the E77 stall's cause: the coordinator did
     * not refuse anything, it simply ran out of permission to speak.
     */
    "peer-nocredit",  /* 21 */
    /*
     * 22 (E82): the emit-time wire-safety guard REFUSED to put a frame on the
     * wire, because it fell outside the envelope every real VMS node in the
     * reference corpus keeps. rc = the port's own PE_VC_SEND_UNSAFE, aux = the
     * `enum cm_guard_class` vector, rendered by name on the line.
     *
     * This line is a BUG REPORT about this node, not about the peer: in a
     * healthy run the FSMs emit correct frames and it never appears. When it
     * does, the frame did NOT go out -- the join fails safe instead of
     * bugchecking a member.
     */
    "unsafe-emit"     /* 22 */
};

/*
 * enum cm_guard_class (the `aux` of an `unsafe-emit` record) --
 * src/kernel-core/vms_cluster_emit_guard.h's own vocabulary, kept
 * byte-identical here for the same reason every other table in this file is:
 * the host drift test compares them ordinal by ordinal, so a name that drifts
 * between the executive and the image that prints it is a RED test rather
 * than a lab-console mystery at 2 a.m.
 */
static const char *const cnxtrace_guard_class_names[] = {
    "-",                     /* 0  CM_GUARD_C_NONE              */
    "envelope-jump",         /* 1  S1  WARN                     */
    "ack-unbacked",          /* 2  S2  DROP -- the CNXMGRERR    */
    "ack-coalesce",          /* 3  S3  DROP -- the INVEXCEPTN   */
    "ack-rate",              /* 4  S4  WARN                     */
    "answered-notification", /* 5  S8  DROP                     */
    "response-txn-zero",     /* 6  S9  DROP                     */
    "conid-zero",            /* 7  S10 DROP                     */
    "frame-size",            /* 8  S11 DROP                     */
    "credit-oversend",       /* 9  S12 DROP                     */
    "envelope-stall"         /* 10     WARN                     */
};

/* enum cnxman_diag_gate (an EMIT record's `detail`) */
static const char *const cnxtrace_gate_names[] = {
    "SENT",          /* 0 */
    "no-csb",        /* 1 */
    "no-open-vc",    /* 2 */
    "no-send-op",    /* 3 */
    "scs-refused",   /* 4 */
    "codec-refused", /* 5 */
    "dialogue-skew"  /* 6 */
};

/* enum cnxman_join_rx */
static const char *const cnxtrace_rx_names[] = {
    "consumed", /* 0 */
    "handoff",  /* 1 */
    "not-mine", /* 2 */
    "bad"       /* 3 */
};

/* enum cnxman_event (src/kernel-core/vms_cnxman.h) */
static const char *const cnxtrace_event_names[] = {
    "CDT_OPEN",       /*  0 */
    "CDT_CLOSED",     /*  1 */
    "RX_MEMBERSHIP",  /*  2 */
    "RX_TR_REQUEST",  /*  3 */
    "RX_TR_RELAY",    /*  4 */
    "RX_TR_OPEN",     /*  5 */
    "RX_TR_GO",       /*  6 */
    "RX_BARRIER",     /*  7 */
    "RX_BARRIER_ACK", /*  8 */
    "RX_REBUILD",     /*  9 */
    "RX_CLOSE",       /* 10 */
    "START",          /* 11 */
    "CSID_LEARNED",   /* 12 */
    "TIMER_RECNX",    /* 13 */
    "TIMER_JOIN",     /* 14 */
    "TIMER_BARRIER",  /* 15 */
    "SHUTDOWN",       /* 16 */
    "RX_TR_ACK",      /* 17 */
    "DIR_RESULT",     /* 18 */
    "MSCP_END",       /* 19 */
    "RX_CONFIG",      /* 20 */
    "RX_COMMIT",      /* 21 */
    "CM_ACCEPTED",    /* 22 */
    "TRANSITION_DONE" /* 23 */
};

#define CNXTRACE_N(a) ((unsigned)(sizeof(a) / sizeof((a)[0])))

/* One bounds-checked lookup, so an ordinal the executive grew and this image
 * has not is rendered "?" rather than read off the end of a table. */
static const char *cnxtrace_name(const char *const *tab, unsigned n,
                                 unsigned char v)
{
    return ((unsigned)v < n) ? tab[v] : "?";
}

#endif /* OVMX_CNXTRACE_NAMES_H */
