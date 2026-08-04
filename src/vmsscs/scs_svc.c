/*
 * scs_svc.c - the five SCS SYSAP services (vms-561).
 *
 * See scs_svc.h for the page cites, the provenance, the honest statement of
 * which of the five have production callers, and the labeled OVMX additions.
 *
 * This file allocates no memory, builds no frame, parses no frame and opens no
 * socket. Every line below is a composition of scs_cdt.c, scs_conn.c,
 * scs_config.c, scs_credit.c and scs_dgram.c.
 */
#include "scs_svc.h"

#include <string.h>

#include "scs_credit.h"
#include "scs_dgram.h"

const char *scs_svc_status_name(enum scs_svc_status st)
{
    switch (st) {
    case SCS_SVC_OK:       return "OK";
    case SCS_SVC_BADARG:   return "BADARG";
    case SCS_SVC_NOCDT:    return "NOCDT";
    case SCS_SVC_NOVC:     return "NOVC";
    case SCS_SVC_BADSTATE: return "BADSTATE";
    case SCS_SVC_NOLISTEN: return "NOLISTEN";
    case SCS_SVC_NOTSENT:  return "NOTSENT";
    }
    return "?";
}

void scs_svc_port_init(struct scs_svc_port *port, struct scs_cdl *cdl)
{
    if (port == NULL) {
        return;
    }
    memset(port, 0, sizeof(*port));
    port->cdl = cdl;
    /* vms-7fe: the "list of listening SYSAPs" IS the p. 2-48 SDIR queue now,
     * and its listening CDTs come from this port's CDL. */
    scs_sdir_queue_init(&port->sdir, cdl);
}

int scs_svc_descriptors_available(const struct scs_svc_port *port)
{
    /* scs_conn_fsm_enabled() re-reads OVMX_NO_CONN_FSM on every call by
     * contract (scs_conn.h), which is what lets a test bracket one service
     * call with setenv/unsetenv and see the mode change. */
    return port != NULL && port->cdl != NULL && scs_conn_fsm_enabled();
}

/* --- LISTEN (p. 2-22, p. 2-48) --------------------------------------------
 *
 * vms-7fe: the fixed `struct scs_svc_listener listen[]` array this used to keep
 * is GONE. p. 2-48 says what the list actually is -- "a queue of SCS Directory
 * Entries (SDIRs) ... Each SDIR contains the CONID of a special 'listening CDT'
 * that is also allocated at this time" -- so the list lives in scs_sdir.c and
 * these three functions are its calling interface. Nothing else changed about
 * LISTEN's contract: same statuses, same not-idempotent duplicate rule.
 */

const struct scs_sdir_queue *scs_svc_sdir(const struct scs_svc_port *port)
{
    return (port != NULL) ? &port->sdir : NULL;
}

struct scs_sdir_queue *scs_svc_sdir_mut(struct scs_svc_port *port)
{
    return (port != NULL) ? &port->sdir : NULL;
}

int scs_svc_listening(const struct scs_svc_port *port, const char *local_sysap)
{
    if (port == NULL) {
        return 0;
    }
    return scs_sdir_peek(&port->sdir, local_sysap) != NULL;
}

enum scs_svc_status scs_listen(struct scs_svc_port *port, const char *local_sysap,
                               scs_svc_connect_req_fn on_connect_req, void *ctx)
{
    if (port == NULL || local_sysap == NULL || local_sysap[0] == '\0') {
        return SCS_SVC_BADARG;
    }
    if (scs_sdir_listen(&port->sdir, local_sysap, on_connect_req, ctx) == NULL) {
        /* Full, duplicate, no CDL, or the reserved listening CONID was taken.
         * All four are "the SYSAP is not listening", which is what the caller
         * has to act on; the SDIR counters distinguish them for the run log. */
        return SCS_SVC_NOLISTEN;
    }
    return SCS_SVC_OK;
}

/* --- the shared machinery ------------------------------------------------- */

/*
 * svc_emit - ask the port to put the action on the wire and account for the
 * answer. Returns one of SCS_SVC_EMIT_*. An action of NONE emits nothing, is
 * not counted either way, and reads as SENT so it never blocks a transition.
 */
static int svc_emit(struct scs_svc_port *port, struct scs_cdt *cdt,
                    enum scs_conn_action action, const struct scs_svc_args *args)
{
    if (action == SCS_CONN_ACT_NONE) {
        return SCS_SVC_EMIT_SENT;
    }
    const char *what = NULL;
    int r = SCS_SVC_EMIT_NOBUILDER;
    if (args->emit != NULL) {
        r = args->emit(args->emit_ctx, cdt, action, args, &what);
    }
    if (r > 0) {
        port->emitted++;
        return SCS_SVC_EMIT_SENT;
    }
    if (r < 0) {
        port->refused++;
        return SCS_SVC_EMIT_REFUSED;
    }
    port->unemitted++;
    return SCS_SVC_EMIT_NOBUILDER;
}

/*
 * svc_step - one transition, emitted then committed.
 *
 * THE ORDER IS THE PRE-MIGRATION ORDER (see scs_svc.h): the pre-vms-561 daemon
 * built and sent the frame and only then called conn_step(). Looking the row up
 * with the PURE table first (scs_conn_table_lookup touches no memory outside
 * its out parameter) is what makes emitting-before-committing possible without
 * the emitter having to guess which frame the machine wants.
 *
 * A REFUSED emit leaves the state EXACTLY where it was -- see the emit contract
 * in scs_svc.h for why that is the pre-migration behaviour and not a new policy.
 */
static enum scs_svc_status svc_step(struct scs_svc_port *port, struct scs_cdt *cdt,
                                    enum scs_conn_event ev,
                                    const struct scs_svc_args *args,
                                    enum scs_conn_state *shadow)
{
    struct scs_conn_transition row;
    /*
     * WHERE `from` COMES FROM. With a CDT it is the CDT's recorded state. With
     * none -- descriptorless mode (scs_svc.h) -- it is the caller's SHADOW
     * state, a plain local that lives only for the duration of one service
     * call.
     *
     * THE SHADOW IS NOT OPTIONAL, and the reason is measured, not theoretical:
     * ACCEPT takes TWO transitions, and without somewhere to remember the first
     * one the second is looked up from CLOSED, finds no (CLOSED, SVC_ACCEPT)
     * row, and scores an illegal event -- which silently DROPS the ACCEPT_REQ.
     * tools/cluster/scsd_wire_diff.sh caught exactly that: two frames vanished
     * from the OVMX_NO_CONN_FSM=1 replay. The shadow is what makes the kill
     * switch mean "record nothing" rather than "send less".
     */
    enum scs_conn_state from = (cdt != NULL) ? scs_conn_state_of(cdt)
                                             : (shadow != NULL ? *shadow : SCS_CONN_CLOSED);
    if (!scs_conn_table_lookup(from, ev, &row)) {
        port->illegal++;
        return SCS_SVC_BADSTATE;
    }

    int emit = svc_emit(port, cdt, row.action, args);
    if (emit == SCS_SVC_EMIT_REFUSED) {
        return SCS_SVC_NOTSENT;
    }

    /* Commit. In descriptorless mode there is nothing to commit to -- that is
     * the whole of the mode: the frames go out, the state is not recorded.
     * scs_conn_fsm_step() is also a no-op under OVMX_NO_CONN_FSM, so this is
     * belt and braces rather than a second policy. */
    if (cdt != NULL) {
        struct scs_conn_transition t = scs_conn_fsm_step(cdt, ev);
        if (!t.suppressed && !t.illegal) {
            port->transitions++;
        }
    } else if (shadow != NULL) {
        *shadow = row.to;
    }
    return SCS_SVC_OK;
}

/*
 * svc_bind - find or allocate the CDT for a connection, and stamp the SYSAP's
 * arguments onto it (pp. 2-28/2-29, 2-42, 2-43).
 *
 * `*fresh` is set to 1 when this call allocated. Returns NULL in descriptorless
 * mode (which is not a failure -- `*status` is SCS_SVC_OK) and NULL with
 * SCS_SVC_NOCDT when the CDL is full or the requested CONID is taken.
 */
static struct scs_cdt *svc_bind(struct scs_svc_port *port, const struct scs_svc_args *args,
                                struct scs_pb *vc, int *fresh,
                                enum scs_svc_status *status)
{
    *fresh = 0;
    *status = SCS_SVC_OK;

    if (!scs_svc_descriptors_available(port)) {
        return NULL; /* DESCRIPTORLESS -- see scs_svc.h */
    }

    /* An existing in-use CDT at the requested CONID IS this connection -- p.
     * 2-29 makes the CONID the CDL index, so a second CDT at the same CONID
     * cannot exist. This is what makes a CONNECT retransmit, or a re-answered
     * CONNECT_REQ, re-drive ONE connection instead of allocating a parallel one.
     *
     * UNLESS IT IS ON A DIFFERENT CIRCUIT, in which case it is a DIFFERENT
     * connection wearing the same CONID, and handing it back would make the CDL
     * describe the wrong one. That is not hypothetical: OVMX's three Con.IDs
     * are node-global macros (scs_cdt.h's KNOWN LIMIT), so a second peer asking
     * to connect lands on exactly this path. It gets SCS_SVC_NOCDT -- the frame
     * still goes out, the connection is simply not tracked, and the caller says
     * so. */
    if (args->conid != 0) {
        struct scs_cdt *have = scs_cdl_lookup(port->cdl, args->conid);
        if (have != NULL && have->in_use) {
            if (vc != NULL && have->pb != NULL && have->pb != vc) {
                *status = SCS_SVC_NOCDT;
                return NULL;
            }
            return have;
        }
    }

    struct scs_cdt *cdt = (args->conid != 0)
        ? scs_cdl_alloc_conid(port->cdl, args->conid, args->local_sysap,
                              args->remote_sysap, vc)
        : scs_cdl_alloc(port->cdl, args->local_sysap, args->remote_sysap, vc);
    if (cdt == NULL) {
        *status = SCS_SVC_NOCDT;
        return NULL;
    }

    *fresh = 1;
    port->cdts_allocated++;
    scs_conn_fsm_init(cdt);
    scs_cdt_set_handlers(cdt, args->msg_input, args->dgram_input, args->vc_loss,
                         args->sysap_ctx);
    if (args->remote_conid != 0) {
        scs_cdt_set_remote_conid(cdt, args->remote_conid);
    }
    if (args->connect_data != NULL && args->connect_data_len != 0) {
        (void)scs_cdt_set_connect_data(cdt, args->connect_data, args->connect_data_len);
    }
    /* p. 2-43 / p. 2-44 / p. 2-42: the optional CONNECT and ACCEPT arguments.
     * Both are no-ops at 0, which is what scsd.c passes today -- OVMX extends no
     * credit and requests no datagram buffers on any production connection. */
    if (args->send_credits != 0 || args->min_send_credits != 0) {
        (void)scs_credit_extend(cdt, args->send_credits, args->min_send_credits);
    }
    if (args->dgram_buffers != 0) {
        (void)scs_dgram_extend(cdt, args->dgram_buffers);
    }
    return cdt;
}

/*
 * svc_release - THE release path, in the vms-17f order minus the VC-loss
 * notification (scs_svc.h says why): flush the Credit Wait (p. 2-45), then
 * scs_cdl_release(), which returns this connection's MFREEQ and DFREEQ deposits
 * to the port (p. 2-43).
 */
static void svc_release(struct scs_svc_port *port, struct scs_cdt *cdt)
{
    if (port == NULL || cdt == NULL || !cdt->in_use) {
        return;
    }
    (void)scs_credit_wait_flush(cdt);
    scs_cdl_release(port->cdl, cdt);
    port->cdts_released++;
}

int scs_svc_close_if_closed(struct scs_svc_port *port, struct scs_cdt *cdt)
{
    if (port == NULL || cdt == NULL || !cdt->in_use) {
        return 0;
    }
    if (scs_conn_state_of(cdt) != SCS_CONN_CLOSED) {
        return 0;
    }
    svc_release(port, cdt);
    return 1;
}

/* --- CONNECT (pp. 2-22..2-23, 2-47, 2-56) --------------------------------- */

enum scs_svc_status scs_connect(struct scs_svc_port *port,
                                const struct scs_svc_args *args,
                                struct scs_cdt **cdt_out)
{
    if (cdt_out != NULL) {
        *cdt_out = NULL;
    }
    if (port == NULL || args == NULL) {
        return SCS_SVC_BADARG;
    }

    /* --- p. 2-47, and it happens BEFORE anything is allocated. "CONNECT ...
     * uses CONFIG_SYS to obtain the address of the first Path Block queued to
     * the System Block for the specified node ... [and] examines each Path
     * Block in turn until it finds one whose virtual circuit is OPEN." */
    struct scs_pb *vc = args->vc;
    if (vc == NULL) {
        if (args->cfg == NULL || args->target_node == NULL) {
            return SCS_SVC_BADARG; /* named neither a circuit nor a node */
        }
        vc = scs_config_select_vc(args->cfg, args->target_node);
    }
    struct scs_config_path_info path;
    if (vc == NULL || !scs_config_path(vc, &path) || path.vc_state != SCS_VC_OPEN) {
        return SCS_SVC_NOVC;
    }

    int fresh = 0;
    enum scs_svc_status bind_st = SCS_SVC_OK;
    struct scs_cdt *cdt = svc_bind(port, args, vc, &fresh, &bind_st);

    /* THE STATE MACHINE IS A RECORDER, NEVER A GATE. If no descriptor could be
     * had -- the CDL is full, or OVMX's node-global CONID is already claimed by
     * another peer (scs_cdt.h's known limit) -- the CONNECT_REQ still goes out
     * and the caller is told the connection is untracked. Suppressing the frame
     * instead would let a bookkeeping shortage take a node out of the cluster. */
    port->connects++;
    enum scs_conn_state shadow = SCS_CONN_CLOSED;
    enum scs_svc_status st = svc_step(port, cdt, SCS_CONN_EV_SVC_CONNECT, args, &shadow);

    /* A CDT for a CONNECT_REQ the port REFUSED to transmit is a lie the CDL
     * would carry for the rest of the run. Unwind exactly what this call
     * allocated. (A NOBUILDER answer is different and is NOT unwound: the
     * connection genuinely advanced, and the stuck-connection diagnostic
     * reporting it parked in CONNECT SENT is the honest outcome.) */
    if (st == SCS_SVC_NOTSENT && fresh) {
        svc_release(port, cdt);
        cdt = NULL;
    }
    if (cdt_out != NULL) {
        *cdt_out = cdt;
    }
    return (st == SCS_SVC_OK) ? bind_st : st;
}

/* --- ACCEPT (p. 2-23, 2-56) ----------------------------------------------- */

enum scs_svc_status scs_accept(struct scs_svc_port *port,
                               const struct scs_svc_args *args,
                               struct scs_cdt **cdt_out)
{
    if (cdt_out != NULL) {
        *cdt_out = NULL;
    }
    if (port == NULL || args == NULL) {
        return SCS_SVC_BADARG;
    }

    /* ACCEPT does not SELECT a circuit -- p. 2-23 puts the ACCEPT_REQ on "the
     * virtual circuit involved in this scenario", the one the CONNECT_REQ
     * arrived on. But it must not accept onto a circuit that is gone (p. 2-31),
     * and refusing BEFORE svc_bind is what stops a broken circuit from
     * allocating a descriptor and advancing a state for a frame that cannot go
     * out. A NULL `vc` is a caller that has not bound the connection to a
     * circuit at all (the two-node test does this); nothing to check. */
    if (args->vc != NULL) {
        struct scs_config_path_info path;
        if (!scs_config_path(args->vc, &path) || path.vc_state != SCS_VC_OPEN) {
            return SCS_SVC_NOVC;
        }
    }

    int fresh = 0;
    enum scs_svc_status bind_st = SCS_SVC_OK;
    enum scs_svc_status st = SCS_SVC_OK;
    struct scs_cdt *cdt = svc_bind(port, args, args->vc, &fresh, &bind_st);
    /* Recorder, never a gate -- see the same note in scs_connect(). */
    /* The peer's handle can arrive on a re-answer too, after the CDT exists. */
    if (cdt != NULL && args->remote_conid != 0) {
        scs_cdt_set_remote_conid(cdt, args->remote_conid);
    }

    port->accepts++;

    /* The descriptorless shadow (see svc_step). A re-answer starts from ACCEPT
     * SENT because that is where the connection would be if anything had been
     * recording it -- the frames must not change just because nothing is. */
    enum scs_conn_state shadow = args->retransmit ? SCS_CONN_ACCEPT_SENT
                                                  : SCS_CONN_CLOSED;
    if (args->retransmit) {
        /* The labeled OVMX row: ACCEPT SENT --RCV_CONNECT_REQ--> ACCEPT SENT,
         * repeating the ACCEPT_REQ. One frame, no second ACCEPT. */
        st = svc_step(port, cdt, SCS_CONN_EV_RCV_CONNECT_REQ, args, &shadow);
    } else {
        /* Figure 2-14's NODE_2 column, both transitions, in wire order. A
         * refusal on the first abandons the second: the connection is not
         * accepted, so no ACCEPT_REQ may follow. */
        st = svc_step(port, cdt, SCS_CONN_EV_RCV_CONNECT_REQ, args, &shadow);
        if (st == SCS_SVC_OK) {
            st = svc_step(port, cdt, SCS_CONN_EV_SVC_ACCEPT, args, &shadow);
        }
    }

    if (st == SCS_SVC_NOTSENT && fresh) {
        svc_release(port, cdt); /* same rule as CONNECT -- see there */
        cdt = NULL;
    }
    if (cdt_out != NULL) {
        *cdt_out = cdt;
    }
    return (st == SCS_SVC_OK) ? bind_st : st;
}

/* --- REJECT (p. 2-24, 2-56) ----------------------------------------------- */

enum scs_svc_status scs_reject(struct scs_svc_port *port,
                               const struct scs_svc_args *args)
{
    if (port == NULL || args == NULL) {
        return SCS_SVC_BADARG;
    }
    /* p. 2-56: "does not involve a CDT at all". No lookup, no allocation, no
     * release -- the CDL is not touched on any path through this function. The
     * action comes from the pure table, which needs no descriptor. */
    struct scs_conn_transition row;
    if (!scs_conn_table_lookup(SCS_CONN_CONNECT_REC, SCS_CONN_EV_SVC_REJECT, &row)) {
        port->illegal++;
        return SCS_SVC_BADSTATE;
    }
    port->rejects++;
    int emit = svc_emit(port, NULL, row.action, args);
    return (emit == SCS_SVC_EMIT_SENT) ? SCS_SVC_OK : SCS_SVC_NOTSENT;
}

/* --- DISCONNECT (pp. 2-25..2-27, 2-56) ------------------------------------ */

enum scs_svc_status scs_disconnect(struct scs_svc_port *port, struct scs_cdt *cdt,
                                   const struct scs_svc_args *args)
{
    static const struct scs_svc_args no_args;
    if (port == NULL) {
        return SCS_SVC_BADARG;
    }
    if (args == NULL) {
        args = &no_args;
    }
    if (cdt == NULL || !cdt->in_use) {
        return SCS_SVC_BADARG;
    }

    port->disconnects++;
    enum scs_svc_status st = svc_step(port, cdt, SCS_CONN_EV_SVC_DISCONNECT, args, NULL);
    if (st == SCS_SVC_BADSTATE) {
        return st;
    }
    /* p. 2-26: one DISCONNECT does not close the connection. Release only if
     * the machine actually reached CLOSED. */
    (void)scs_svc_close_if_closed(port, cdt);
    return st;
}
