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
    "codec refused to build"         /* 7 */
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
    "cdt-closed"      /* 12 */
};

/* enum cnxman_diag_gate (an EMIT record's `detail`) */
static const char *const cnxtrace_gate_names[] = {
    "SENT",          /* 0 */
    "no-csb",        /* 1 */
    "no-open-vc",    /* 2 */
    "no-send-op",    /* 3 */
    "scs-refused",   /* 4 */
    "codec-refused"  /* 5 */
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
    "CM_ACCEPTED"     /* 22 */
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
