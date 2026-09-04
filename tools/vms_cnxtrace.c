/*
 * vms_cnxtrace.c - SYS$SYSTEM:CNXTRACE.EXE, the connection manager's JOIN
 * TRANSITION RING, printed to SYS$OUTPUT (E69).
 *
 * ===========================================================================
 * WHY THIS IMAGE EXISTS
 *
 * The OVMX cluster stack is executive-resident (vms.ko / vms.kmod) and the
 * executive has no console log -- its output is not OPA0: (src/ovmx_init/
 * ovmx_init.c:1399). So when a join stalls, the ONLY evidence has been the
 * wire, and three consecutive promotion walls against the live 2-node VAX
 * cluster (integration notes E67, E68 and the E68 re-fire) were diagnosed from
 * a pcap by luck. The remaining gap is wire-INVISIBLE by construction: OVMX
 * receives the members' PARAMS and never answers with its own MODEL/PARAMS
 * burst, and no frame anywhere says why.
 *
 * This image is how that evidence gets out. It opens /dev/vms, issues the
 * read-only VMS_IOCTL_CLUSTER_DIAG_JOIN, and prints the executive's own
 * transition transcript one record per line -- so a lab console capture
 * (ovmx-node-*.log) records what the join FSM actually did.
 *
 * ===========================================================================
 * WHAT IT WILL AND WILL NOT PRINT (INV-6)
 *
 * Every column below is a value the EXECUTIVE recorded at a real dispatch or a
 * real emit attempt. This image computes nothing, infers nothing and defaults
 * nothing: it renders ordinals as names and longwords as hex.
 *
 * When the executive answers SS$_NOSUCHDEV -- no /dev/vms, VAXCLUSTER=0, or no
 * CLUSTER_START yet -- it says exactly that and prints NO records. An all-zero
 * view is not an empty transcript; rendering one as "the join did nothing"
 * would be the fabrication this whole instrument exists to prevent.
 *
 * ===========================================================================
 * USAGE
 *
 *     $ RUN SYS$SYSTEM:CNXTRACE.EXE
 *
 * Prints every record the ring still holds, oldest first. An optional argument
 * caps how many records are printed (a full 256-record ring is ~28 KB over a
 * serial console).
 *
 * A line ending in `rep=N tlast=T` is N occurrences of ONE identical fact,
 * back to back, the last at T -- the executive coalesces those rather than let
 * a once-a-second watchdog wrap the join drive out of the ring.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssdef.h"          /* SS$_NORMAL / SS$_NOSUCHDEV -- the ONE table */
#include "vms_kif.h"        /* vms_kif_cluster_diag_join + the args struct */
#include "cnxtrace_names.h" /* the ordinal->name tables (drift-gated) */

/* Mirrors of the two kernel-core vocabularies this renderer switches on
 * (src/kernel-core/vms_cnxman_diag.h). Only the values whose MEANING changes
 * the rendering are named here; everything else is looked up by ordinal. */
#define CNXTRACE_K_DISPATCH 0u
#define CNXTRACE_K_ARRIVAL  1u
#define CNXTRACE_K_EMIT     2u
#define CNXTRACE_EV_NONE    0xffu
#define CNXTRACE_CAT_MSCP   0xffu
/* E70's named port refusals (enum cnxman_diag_reason 14..19). Named here for
 * the same reason the three kinds are: their `aux` column MEANS something
 * different per reason, and a reader on a lab console should not have to hold
 * the mapping in their head. */
#define CNXTRACE_R_PORT_NOCIRCUIT   14u
#define CNXTRACE_R_PORT_NOCREDIT    15u
#define CNXTRACE_R_PORT_RINGFULL    16u
#define CNXTRACE_R_CDT_NOT_SENDABLE 19u
#define CNXTRACE_NO_VC      0xffffffffu

/*
 * The join states are the executive's own strings and two of them contain a
 * space ("DIR ROUND", "MSCP CONNECT"). The tables must stay byte-identical to
 * the executive's -- that is what the host drift test compares -- so the SPACE
 * is removed here, at render time, to keep every output line a clean sequence
 * of key=value tokens that `awk` and `grep` can split on whitespace.
 */
static void cnxtrace_token(const char *name, char *out, size_t cap)
{
    size_t i;

    for (i = 0; i + 1u < cap && name[i] != '\0'; i++)
        out[i] = (name[i] == ' ') ? '_' : name[i];
    out[i] = '\0';
}

/* CNXTRACE_EV_NONE is the executive's own "this record is not about a shared
 * event" and BOTH sides render it "-". Everything else is a bounded table
 * lookup, so an ordinal the executive grew and this image has not prints "?"
 * rather than reading off the end. */
static const char *cnxtrace_event(unsigned char ev)
{
    if (ev == CNXTRACE_EV_NONE)
        return "-";
    return cnxtrace_name(cnxtrace_event_names,
                         CNXTRACE_N(cnxtrace_event_names), ev);
}

/*
 * `rx` is the answer the [state][event] TABLE gave, so it exists only on a
 * DISPATCH record. Printing the other kinds' zero as "consumed" would be a
 * fabricated column -- the same reason cat/op are printed only for EMIT.
 */
static const char *cnxtrace_rx(const struct cnxman_diag_rec_wire *r)
{
    if (r->kind != CNXTRACE_K_DISPATCH)
        return "-";
    return cnxtrace_name(cnxtrace_rx_names, CNXTRACE_N(cnxtrace_rx_names),
                         r->rx);
}

static const char *cnxtrace_state(unsigned char s, char *buf, size_t cap)
{
    cnxtrace_token(cnxtrace_name(cnxtrace_state_names,
                                 CNXTRACE_N(cnxtrace_state_names), s),
                   buf, cap);
    return buf;
}

/*
 * `detail` means a different thing per record kind -- that is the point of the
 * three kinds -- so it is rendered by the kind and never by a shared table.
 */
static const char *cnxtrace_detail(const struct cnxman_diag_rec_wire *r)
{
    switch (r->kind) {
    case CNXTRACE_K_DISPATCH:
        /* The empty table cells are the interesting ones: an event the FSM had
         * no edge for in the state it arrived in (its own `ignored_events`). */
        return r->detail ? "fired" : "EMPTY-CELL";
    case CNXTRACE_K_ARRIVAL:
        return cnxtrace_name(cnxtrace_reason_names,
                             CNXTRACE_N(cnxtrace_reason_names), r->detail);
    case CNXTRACE_K_EMIT:
        return cnxtrace_name(cnxtrace_gate_names,
                             CNXTRACE_N(cnxtrace_gate_names), r->detail);
    default:
        return "?";
    }
}

/*
 * WHAT `aux` MEANS ON THIS RECORD, in words (E70). The column is already
 * printed; this adds the ONE token that says which quantity it is, and only for
 * the reasons where `aux` is not simply a Con.ID. Everything it prints comes
 * out of the record -- nothing is recomputed, and a reason without a special
 * meaning gets no token at all rather than a guessed one.
 */
static void cnxtrace_print_aux_note(const struct cnxman_diag_rec_wire *r)
{
    if (r->kind != CNXTRACE_K_ARRIVAL)
        return;
    switch (r->detail) {
    case CNXTRACE_R_PORT_NOCIRCUIT:
        if (r->aux == CNXTRACE_NO_VC)
            printf(" vc=NONE");          /* no circuit object at all */
        else
            printf(" vcstate=%u", (unsigned)r->aux);
        break;
    case CNXTRACE_R_PORT_NOCREDIT:
        printf(" refused_credit=%u", (unsigned)r->aux);
        break;
    case CNXTRACE_R_PORT_RINGFULL:
        printf(" refused_ring=%u", (unsigned)r->aux);
        break;
    case CNXTRACE_R_CDT_NOT_SENDABLE:
        /* rc is the CDT's live state; aux is its live Send Credit. */
        printf(" cdtstate=%d credit_send=%u", (int)r->rc, (unsigned)r->aux);
        break;
    default:
        break;
    }
}

/* One record, one line, all key=value. */
static void cnxtrace_print_rec(const struct cnxman_diag_rec_wire *r)
{
    char from[32], to[32];

    printf("CNXTRACE seq=%08u t=%08u kind=%-8s state=%s->%s ev=%s "
           "detail=%s rx=%s rc=%d aux=0x%08x",
           (unsigned)r->seq, (unsigned)r->t_ms,
           cnxtrace_name(cnxtrace_kind_names,
                         CNXTRACE_N(cnxtrace_kind_names), r->kind),
           cnxtrace_state(r->state, from, sizeof(from)),
           cnxtrace_state(r->new_state, to, sizeof(to)),
           cnxtrace_event(r->event),
           cnxtrace_detail(r),
           cnxtrace_rx(r),
           (int)r->rc, (unsigned)r->aux);

    /* The wire bytes, printed only for the kind that really carries them --
     * a cat/op column on a DISPATCH record would be two fabricated zeros. */
    if (r->kind == CNXTRACE_K_EMIT) {
        if (r->cat == CNXTRACE_CAT_MSCP)
            printf(" cat=MSCP op=0x%02x", (unsigned)r->op);
        else
            printf(" cat=0x%02x op=0x%02x", (unsigned)r->cat,
                   (unsigned)r->op);
    }

    cnxtrace_print_aux_note(r);

    /* The coalescing columns, printed ONLY when the fact really did repeat
     * (kernel-core vms_cnxman_diag.h SS4b). A `rep=0 tlast=` on every line
     * would be two columns of noise on the 99% of records that happened once
     * -- and would say nothing the `t` column does not already say. */
    if (r->repeat != 0u)
        printf(" rep=%u tlast=%08u", (unsigned)r->repeat + 1u,
               (unsigned)r->t_last_ms);
    printf("\n");
}

static void cnxtrace_print_header(const struct cnxman_diag_view_wire *v)
{
    char st[32];

    printf("%%CNXTRACE-I-BEGIN, CNXMAN join transition ring "
           "(executive-resident, read-only)\n");
    printf("%%CNXTRACE-I-STATE, join state=%s failure=%s ignored_events=%u "
           "recording=%s\n",
           cnxtrace_state(v->join_state, st, sizeof(st)),
           cnxtrace_name(cnxtrace_failure_names,
                         CNXTRACE_N(cnxtrace_failure_names), v->join_failure),
           (unsigned)v->ignored_events, v->enabled ? "yes" : "no");
    printf("%%CNXTRACE-I-COUNT, held=%u recorded=%u dropped=%u\n",
           (unsigned)v->count, (unsigned)v->recorded,
           (unsigned)(v->recorded - v->count));
}

/*
 * Walk the ring. One ioctl per window of CNXMAN_DIAG_ROWS records; `first`
 * advances by what the executive really returned, so a ring that wraps between
 * two calls is reported short rather than duplicated -- the sequence column is
 * what a reader checks for a gap.
 */
static unsigned cnxtrace_walk(unsigned limit)
{
    struct vms_cluster_diag_join_args args;
    unsigned first = 0u, printed = 0u;

    for (;;) {
        unsigned i;
        uint32_t st;

        memset(&args, 0, sizeof(args));
        args.first = first;
        st = vms_kif_cluster_diag_join(&args);
        if (st != SS$_NORMAL) {
            printf("%%CNXTRACE-W-READ, the executive refused the ring read "
                   "at record %u (SS$ %u)\n", first, (unsigned)st);
            break;
        }
        if (first == 0u)
            cnxtrace_print_header(&args.view);
        if (args.view.n_rows == 0u)
            break;

        for (i = 0; i < args.view.n_rows; i++) {
            if (limit != 0u && printed >= limit)
                return printed;
            cnxtrace_print_rec(&args.view.row[i]);
            printed++;
        }
        first += args.view.n_rows;
    }
    return printed;
}

int main(int argc, char **argv)
{
    struct vms_cluster_diag_join_args probe;
    unsigned limit = 0u, printed;
    uint32_t st;

    if (argc > 1)
        limit = (unsigned)strtoul(argv[1], NULL, 10);

    /*
     * Probe FIRST, so the honest "there is no connection manager" answer is a
     * single named line and not a header followed by nothing. SS$_NOSUCHDEV is
     * a real, common state (no /dev/vms, VAXCLUSTER=0, before CLUSTER_START)
     * and it is reported as itself.
     */
    memset(&probe, 0, sizeof(probe));
    st = vms_kif_cluster_diag_join(&probe);
    if (st == SS$_NOSUCHDEV) {
        printf("%%CNXTRACE-W-NOCNXMAN, the connection manager is not started "
               "on this node: there is no join transcript to print\n");
        return 1;
    }
    if (st != SS$_NORMAL) {
        printf("%%CNXTRACE-E-NOREAD, the executive refused the join "
               "diagnostics read (SS$ %u)\n", (unsigned)st);
        return 1;
    }

    printed = cnxtrace_walk(limit);
    printf("%%CNXTRACE-I-END, %u records printed\n", printed);
    return 0;
}
