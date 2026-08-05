/*
 * scs_poll.c - the SCS Process Poller (vms-66f), p. 2-50.
 * See scs_poll.h for the page-by-page requirement list and the GROUNDED
 * PRCPOLINTERVAL measurement.
 */
#include "scs_poll.h"

/* vms-096: scs_poll_pb_departing() mirrors scs_pb_depart()'s kill switch, so
 * this file needs scs_depart_enabled(). It links no other symbol from there. */
#include "scs_depart.h"

#include <stdlib.h>
#include <string.h>

/* --- name helpers ---------------------------------------------------------
 *
 * SYSAP names travel the wire as 16-byte blank-padded fields and arrive from
 * scs_dir_parse() in that form, while callers pass C strings. Comparing them
 * has to ignore the padding on both sides or "VMS$VAXcluster" would never match
 * "VMS$VAXcluster  ". */
static size_t name_trim_len(const char *s)
{
    size_t n = 0;
    while (n < SCS_DIR_NAME_LEN && s[n] != '\0') {
        n++;
    }
    while (n > 0 && s[n - 1] == ' ') {
        n--;
    }
    return n;
}

static int name_eq(const char *a, const char *b)
{
    size_t la = name_trim_len(a);
    size_t lb = name_trim_len(b);
    return la == lb && la > 0 && memcmp(a, b, la) == 0;
}

static void name_copy(char dst[SCS_DIR_NAME_LEN + 1], const char *src)
{
    size_t n = name_trim_len(src);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

const char *scs_poll_state_name(enum scs_poll_state s)
{
    switch (s) {
    case SCS_POLL_IDLE:          return "IDLE";
    case SCS_POLL_CONNECTING:    return "CONNECTING";
    case SCS_POLL_INQUIRING:     return "INQUIRING";
    case SCS_POLL_DISCONNECTING: return "DISCONNECTING";
    }
    return "?";
}

/*
 * THE POLLER IS OFF UNLESS ASKED FOR, AND THE REASON IS A MEASUREMENT.
 *
 * Bracketed back-to-back on the reference lab (lab-2 replica `vaxlab-1`, VAX
 * 7.3 node VAX1, 2026-08-05, same binary, ~60 s each, logs kept at
 * /data/training/vax/k8s-labs/vaxlab-1/logs/vms66f-{on,off}.log):
 *
 *   poller RUNNING : connects-sent=8 inquiries-sent=1 answers=0
 *                    -> PEER dir_connected=no dir_lookups=0 cm_config=no
 *                       conn[dir=untracked joiner=untracked]   NO JOIN
 *   poller GATED   : connects-sent=0 inquiries-sent=0
 *                    -> PEER dir_connected=YES dir_lookups=4 cm_config=YES
 *                       conn[dir=OPEN joiner=OPEN]             JOIN COMPLETED
 *
 * So on this base the poller does not merely add traffic -- it COSTS THE JOIN.
 * With OVMX opening its own SCS$DIRECTORY connection the instant the circuit
 * comes up, the member never ran its own directory phase at all
 * (connect_scans=0), and the whole VMS$VAXcluster handshake that depends on it
 * never started. Two further facts from the same capture bound what is and is
 * not broken:
 *   - the VAX DOES accept an OVMX-initiated SCS$DIR_LOOKUP -> SCS$DIRECTORY
 *     connection: it answered with a message-type-1 CONNECT_RSP and, on the
 *     second cycle, a message-type-2 ACCEPT_REQ, taking OVMX's CDT to OPEN;
 *   - it did NOT answer the inquiry that followed (answers=0 over 8 cycles).
 * Both are recorded in docs/cluster-protocol-spec.md sec 4(h).
 *
 * Shipping this on by default would therefore trade a working join for a
 * dialogue the peer does not complete. It ships OFF, opt-in via
 * OVMX_PROCESS_POLLER=1, until the unanswered-inquiry gap is closed --
 * and OVMX_NO_PROCESS_POLLER=1 still forces it off from any state, which is
 * the kill-switch the wire-visible-change rule requires and which the two runs
 * above are the measurement of.
 */
int scs_poll_enabled(void)
{
    const char *off = getenv("OVMX_NO_PROCESS_POLLER");
    if (off != NULL && off[0] == '1') {
        return 0; /* the kill-switch wins from any state */
    }
    const char *on = getenv("OVMX_PROCESS_POLLER");
    return on != NULL && on[0] == '1';
}

unsigned scs_poll_interval_sec(void)
{
    const char *e = getenv("OVMX_PRCPOLINTERVAL");
    if (e == NULL || e[0] == '\0') {
        return SCS_POLL_PRCPOLINTERVAL_DEFAULT;
    }
    long v = strtol(e, NULL, 10);
    /* Clamp to the SYSGEN range GROUNDED in scs_poll.h rather than honouring an
     * out-of-range value SYSGEN itself would refuse. */
    if (v < (long)SCS_POLL_PRCPOLINTERVAL_MIN) {
        return SCS_POLL_PRCPOLINTERVAL_MIN;
    }
    if (v > (long)SCS_POLL_PRCPOLINTERVAL_MAX) {
        return SCS_POLL_PRCPOLINTERVAL_MAX;
    }
    return (unsigned)v;
}

void scs_poll_init(struct scs_poller *p, struct scs_svc_port *port,
                   struct scs_config *cfg)
{
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof(*p));
    p->port = port;
    p->cfg = cfg;
    p->state = SCS_POLL_IDLE;
}

void scs_poll_set_emitters(struct scs_poller *p,
                           scs_svc_emit_fn emit, void *emit_ctx,
                           scs_poll_inquiry_fn inquire, void *inquire_ctx)
{
    if (p == NULL) {
        return;
    }
    p->emit = emit;
    p->emit_ctx = emit_ctx;
    p->inquire = inquire;
    p->inquire_ctx = inquire_ctx;
}

static struct scs_poll_name *name_find(struct scs_poller *p, const char *sysap)
{
    for (unsigned i = 0; i < SCS_POLL_MAX_NAMES; i++) {
        if (p->names[i].in_use && name_eq(p->names[i].sysap, sysap)) {
            return &p->names[i];
        }
    }
    return NULL;
}

int scs_poll_request(struct scs_poller *p, const char *sysap,
                     const uint8_t node[6], scs_poll_found_fn on_found, void *ctx)
{
    if (p == NULL || sysap == NULL || name_trim_len(sysap) == 0) {
        return 0;
    }
    struct scs_poll_name *n = name_find(p, sysap);
    if (n == NULL) {
        for (unsigned i = 0; i < SCS_POLL_MAX_NAMES; i++) {
            if (!p->names[i].in_use) {
                n = &p->names[i];
                break;
            }
        }
        if (n == NULL) {
            return 0;
        }
        memset(n, 0, sizeof(*n));
        n->in_use = 1;
        name_copy(n->sysap, sysap);
    }
    /* p. 2-50: a fresh request re-enables polling -- this is the "once again
     * requesting its Process Poller to look for SYSAP_X on NODE_X" path. */
    n->disabled_count = 0;
    n->on_found = on_found;
    n->ctx = ctx;
    if (node == NULL) {
        n->all_nodes = 1;
        memset(n->node, 0, 6);
    } else {
        n->all_nodes = 0;
        memcpy(n->node, node, 6);
    }
    return 1;
}

int scs_poll_add_node(struct scs_poller *p, const uint8_t node[6])
{
    if (p == NULL || node == NULL) {
        return 0;
    }
    for (unsigned i = 0; i < SCS_POLL_MAX_NODES; i++) {
        if (p->nodes[i].in_use && mac_eq(p->nodes[i].addr, node)) {
            return 1; /* already visible; keep its cadence */
        }
    }
    for (unsigned i = 0; i < SCS_POLL_MAX_NODES; i++) {
        if (!p->nodes[i].in_use) {
            memset(&p->nodes[i], 0, sizeof(p->nodes[i]));
            p->nodes[i].in_use = 1;
            memcpy(p->nodes[i].addr, node, 6);
            return 1;
        }
    }
    return 0;
}

void scs_poll_drop_node(struct scs_poller *p, const uint8_t node[6])
{
    if (p == NULL || node == NULL) {
        return;
    }
    for (unsigned i = 0; i < SCS_POLL_MAX_NODES; i++) {
        if (p->nodes[i].in_use && mac_eq(p->nodes[i].addr, node)) {
            /* If the cycle in flight is against this node it cannot continue --
             * the circuit that carried it is gone. */
            if (p->state != SCS_POLL_IDLE && mac_eq(p->cur_node, node)) {
                scs_poll_abandon(p);
            }
            memset(&p->nodes[i], 0, sizeof(p->nodes[i]));
        }
    }
}

static int name_disabled_on(const struct scs_poll_name *n, const uint8_t node[6])
{
    for (unsigned i = 0; i < n->disabled_count; i++) {
        if (mac_eq(n->disabled[i], node)) {
            return 1;
        }
    }
    return 0;
}

/* Does this registration apply to `node` at all (p. 2-50 specific-node scope)? */
static int name_scopes(const struct scs_poll_name *n, const uint8_t node[6])
{
    return n->all_nodes || mac_eq(n->node, node);
}

void scs_poll_connected(struct scs_poller *p, const char *sysap, const uint8_t node[6])
{
    if (p == NULL || sysap == NULL || node == NULL) {
        return;
    }
    struct scs_poll_name *n = name_find(p, sysap);
    if (n == NULL || name_disabled_on(n, node)) {
        return;
    }
    if (n->disabled_count < SCS_POLL_MAX_NODES) {
        memcpy(n->disabled[n->disabled_count], node, 6);
        n->disabled_count++;
    }
}

void scs_poll_connection_lost(struct scs_poller *p, const char *sysap, const uint8_t node[6])
{
    if (p == NULL || sysap == NULL || node == NULL) {
        return;
    }
    struct scs_poll_name *n = name_find(p, sysap);
    if (n == NULL) {
        return;
    }
    for (unsigned i = 0; i < n->disabled_count; i++) {
        if (mac_eq(n->disabled[i], node)) {
            memmove(n->disabled[i], n->disabled[i + 1],
                    (size_t)(n->disabled_count - i - 1) * 6);
            n->disabled_count--;
            return;
        }
    }
}

int scs_poll_polling(const struct scs_poller *p, const char *sysap, const uint8_t node[6])
{
    if (p == NULL || sysap == NULL || node == NULL) {
        return 0;
    }
    for (unsigned i = 0; i < SCS_POLL_MAX_NAMES; i++) {
        const struct scs_poll_name *n = &p->names[i];
        if (!n->in_use || !name_eq(n->sysap, sysap)) {
            continue;
        }
        return name_scopes(n, node) && !name_disabled_on(n, node);
    }
    return 0;
}

enum scs_poll_state scs_poll_state_of(const struct scs_poller *p)
{
    return p == NULL ? SCS_POLL_IDLE : p->state;
}

unsigned scs_poll_pending(const struct scs_poller *p)
{
    return p == NULL ? 0u : p->pending_count;
}

const uint8_t *scs_poll_current_node(const struct scs_poller *p)
{
    return p == NULL ? NULL : p->cur_node;
}

/*
 * Give the cycle's descriptor back.
 *
 * p. 2-26 releases a CDT when the connection reaches CLOSED, and
 * scs_svc_close_if_closed() is that path. THIS IS THE OTHER PATH -- the one
 * taken when a cycle ends WITHOUT the dialogue completing, which is what
 * scs_poll_abandon() is for: a timeout, a lost circuit, a node dropped under a
 * cycle in flight.
 *
 * A poller that simply kept the descriptor would poll exactly ONCE per boot:
 * the next cycle's CONNECT reuses the same Con.ID, the state machine has no
 * CONNECT rule from DISC SENT, and every later cycle is refused. That was
 * measured, not assumed -- with the descriptor retained, cycle 2 and cycle 3
 * both landed in connect_refused.
 *
 * So the poller force-releases, and COUNTS it in descriptors_forced rather than
 * hiding it in the normal release path.
 *
 * CORRECTED (round 4). An earlier revision of this comment said OVMX "builds no
 * DISCONNECT_REQ and no DISCONNECT_RSP (scs_svc.h), so the symmetric dialogue
 * never completes", and predicted that "when vms-591 lands, descriptors_forced
 * should fall to zero on its own with no change here". vms-591 HAS landed --
 * scs_disc_build_request()/scs_disc_build_response() exist -- and the
 * prediction was WRONG TWICE. It needed a change in scsd.c (the poller's
 * emitter still answered NOBUILDER for SEND_DISCONNECT_REQ, so no teardown
 * frame left the daemon at all), and it needed scs_poll_cdt_released() below,
 * because the peer's answers are delivered by the RECEIVE path, which releases
 * the descriptor itself. With both in place a completed cycle now leaves
 * through the clean arms and descriptors_forced stays at 0; the daemon wire
 * test asserts exactly that.
 */
static void poll_release_cdt(struct scs_poller *p)
{
    if (p->cdt == NULL) {
        return;
    }
    /* vms-096, DEFENCE IN DEPTH against a RECYCLED-SLOT RELEASE. A CDT this
     * poller no longer owns must never be released here.
     *
     * scs_cdl_release() memsets the descriptor, so a released-and-not-yet-reused
     * slot reads in_use == 0. A slot released and then handed to a DIFFERENT
     * connection reads a different SYSAP pair: the poller's connection is always
     * SCS$DIR_LOOKUP -> SCS$DIRECTORY (see poll_args()), and p. 2-50's "only one
     * node will be polled at a time" means there is never a second one.
     *
     * NOT local_conid: that is derived from the CDL SLOT INDEX
     * (SCS_CDT_MAKE_CONID in scs_cdt.c), so EVERY connection that lands in the
     * recycled slot carries the same value and the check would pass for all of
     * them. Measured -- the first version of this guard used local_conid and the
     * recycled-slot arm of test_peer_departure_under_a_live_poll_cycle() still
     * released the victim.
     *
     * HONEST RESIDUAL: a slot reused by ANOTHER SCS$DIR_LOOKUP -> SCS$DIRECTORY
     * connection would still pass. The primary fix -- scs_poll_pb_departing(),
     * which stops the poller pointing at a released descriptor at all -- is what
     * closes that; this check is what makes a MISSED notification harmless
     * against every other connection rather than destructive. */
    if (!p->cdt->in_use ||
        !name_eq(p->cdt->local_sysap, SCS_DIR_SYSAP_POLLER) ||
        !name_eq(p->cdt->remote_sysap, SCS_DIR_SYSAP_DIRECTORY)) {
        p->cdt = NULL;
        return;
    }
    if (!scs_svc_close_if_closed(p->port, p->cdt)) {
        (void)scs_credit_wait_flush(p->cdt); /* p. 2-45, same order as scs_svc.c */
        if (p->port->cdl != NULL) {
            scs_cdl_release(p->port->cdl, p->cdt);
            p->port->cdts_released++;
        }
        p->descriptors_forced++;
    }
    p->cdt = NULL;
}

void scs_poll_abandon(struct scs_poller *p)
{
    if (p == NULL) {
        return;
    }
    poll_release_cdt(p);
    p->state = SCS_POLL_IDLE;
    p->pending_count = 0;
    memset(p->cur_node, 0, 6);
}

/*
 * The receive path reached CLOSED on a connection and released its descriptor.
 * scs_poll.h carries why this is pushed rather than polled. NOTHING IS RELEASED
 * HERE -- the caller already did that, and calling scs_cdl_release() a second
 * time on a recycled slot is exactly the bug this function exists to avoid.
 *
 * The two cycle states are counted APART because they are different events. In
 * DISCONNECTING this is p. 2-50's cycle ending the way the page describes and
 * p. 2-26's release arriving to prove it. In CONNECTING or INQUIRING it is the
 * DIRECTORY disconnecting first, mid-cycle: the inquiries that were still
 * outstanding never got their replies, which is an abandoned cycle however
 * politely the connection came down.
 */
int scs_poll_cdt_released(struct scs_poller *p, const struct scs_cdt *cdt)
{
    if (p == NULL || cdt == NULL || p->cdt != cdt) {
        return 0;
    }
    if (p->state == SCS_POLL_DISCONNECTING) {
        p->disconnects_closed++;
    } else if (p->state != SCS_POLL_IDLE) {
        p->cycles_abandoned++;
    }
    p->cdt = NULL;
    p->state = SCS_POLL_IDLE;
    p->pending_count = 0;
    memset(p->cur_node, 0, 6);
    return 1;
}

/*
 * vms-096: the departure sweep is about to release every CDT on `pb`, and one
 * of them may be this poller's in-flight cycle. See scs_poll.h for the
 * recycled-slot release this prevents.
 *
 * Deliberately does NOT release anything: scs_pb_depart() is the releaser, and
 * a second scs_cdl_release() on the same descriptor is the other half of the
 * same bug. It only forgets the descriptor and ends the cycle, which is exactly
 * what scs_poll_cdt_released() does once the release has happened.
 */
int scs_poll_pb_departing(struct scs_poller *p, const struct scs_pb *pb)
{
    if (p == NULL || pb == NULL || p->cdt == NULL || p->cdt->pb != pb) {
        return 0;
    }
    /* Mirror scs_pb_depart()'s own preconditions exactly: if it is going to
     * decline (kill switch set, or the Path Block is not in use) it releases
     * NOTHING, and ending a live cycle on a teardown that did not happen would
     * be a second, opposite defect. */
    if (!pb->in_use || !scs_depart_enabled()) {
        return 0;
    }
    /* A cycle taken down by a node dropping out is ABANDONED however far it had
     * got -- even in DISCONNECTING, where the peer's answer is now never coming.
     * That is the difference from scs_poll_cdt_released(), which counts a
     * DISCONNECTING release as the p. 2-26 close it really is. */
    if (p->state != SCS_POLL_IDLE) {
        p->cycles_abandoned++;
    }
    p->cdt = NULL;
    p->state = SCS_POLL_IDLE;
    p->pending_count = 0;
    memset(p->cur_node, 0, 6);
    return 1;
}

/* How many enabled names apply to `node`? A node with none is not worth a
 * connection -- p. 2-50's cycle exists to carry inquiries. */
static unsigned enabled_names_for(struct scs_poller *p, const uint8_t node[6])
{
    unsigned c = 0;
    for (unsigned i = 0; i < SCS_POLL_MAX_NAMES; i++) {
        struct scs_poll_name *n = &p->names[i];
        if (n->in_use && name_scopes(n, node) && !name_disabled_on(n, node)) {
            c++;
        }
    }
    return c;
}

static void poll_args(struct scs_poller *p, struct scs_svc_args *a,
                      const uint8_t node[6])
{
    memset(a, 0, sizeof(*a));
    a->local_sysap = SCS_DIR_SYSAP_POLLER;    /* p. 2-50: SCS$DIR_LOOKUP */
    a->remote_sysap = SCS_DIR_SYSAP_DIRECTORY;/* p. 2-50: SCS$DIRECTORY */
    a->target_node = node;                    /* p. 2-47: CONNECT selects the VC */
    a->cfg = p->cfg;
    a->conid = SCS_DIR_OVMX_POLL_CONID;
    a->remote_conid = 0;                      /* not known until the answer */
    a->emit = p->emit;
    a->emit_ctx = p->emit_ctx;
}

void scs_poll_tick(struct scs_poller *p, uint64_t now_ms)
{
    if (p == NULL || p->port == NULL) {
        return;
    }
    if (!scs_poll_enabled()) {
        return; /* kill-switch: no CONNECT, no inquiry, nothing built */
    }

    /* A cycle in flight: only the completion retry and the abandon timer run.
     * p. 2-50's one-node-at-a-time rule is this early return, not a check on a
     * counter. */
    if (p->state != SCS_POLL_IDLE) {
        /* THE CLEAN ENDING, retried. The connection reached CLOSED and nobody
         * has released the descriptor yet, so the poller does it the p. 2-26
         * way and the cycle ends without touching descriptors_forced.
         *
         * WHEN THIS ARM RUNS RATHER THAN scs_poll_cdt_released(). Whoever
         * drives the final receive event decides. scs_svc_deliver() releases as
         * it closes, and the daemon pushes that release through
         * scs_poll_cdt_released(); a caller that steps the machine directly --
         * scsd.c's conn_step() fallback for a frame whose peer slot is gone,
         * and scs_vc_break()'s p. 2-28 VC_LOST sweep -- reaches CLOSED and
         * releases NOTHING, and this is where those descriptors come back. */
        if (p->state == SCS_POLL_DISCONNECTING && p->cdt != NULL &&
            scs_svc_close_if_closed(p->port, p->cdt)) {
            p->cdt = NULL;
            p->state = SCS_POLL_IDLE;
            p->disconnects_closed++;
            memset(p->cur_node, 0, 6);
            return;
        }
        if (now_ms >= p->cycle_start_ms &&
            now_ms - p->cycle_start_ms >= SCS_POLL_CYCLE_TIMEOUT_MS) {
            /* Two different failures, counted apart. A cycle abandoned while
             * still CONNECTING/INQUIRING never got its answers; one abandoned in
             * DISCONNECTING got them all and the disconnect dialogue never
             * finished. Collapsing them would make the second look like a
             * discovery failure. */
            if (p->state == SCS_POLL_DISCONNECTING) {
                p->disconnects_unclosed++;
            } else {
                p->cycles_abandoned++;
            }
            scs_poll_abandon(p);
        }
        return;
    }

    /* "one of them will have to wait approximately one second" (p. 2-50). */
    if (p->last_cycle_ms != 0 && now_ms >= p->last_cycle_ms &&
        now_ms - p->last_cycle_ms < SCS_POLL_ONE_AT_A_TIME_MS) {
        return;
    }

    uint64_t interval_ms = (uint64_t)scs_poll_interval_sec() * 1000u;

    /* Round-robin from next_scan so a due node cannot be starved by an earlier
     * slot that is due every time. */
    int chosen = -1;
    for (unsigned k = 0; k < SCS_POLL_MAX_NODES; k++) {
        unsigned i = (p->next_scan + k) % SCS_POLL_MAX_NODES;
        struct scs_poll_node *nd = &p->nodes[i];
        if (!nd->in_use) {
            continue;
        }
        if (nd->polled_once && (now_ms < nd->last_poll_ms ||
                                now_ms - nd->last_poll_ms < interval_ms)) {
            continue; /* "not more than once during each such interval" */
        }
        unsigned want = enabled_names_for(p, nd->addr);
        if (want == 0) {
            p->skipped_disabled++;
            continue;
        }
        chosen = (int)i;
        break;
    }
    if (chosen < 0) {
        return;
    }

    struct scs_poll_node *nd = &p->nodes[chosen];
    struct scs_svc_args a;
    poll_args(p, &a, nd->addr);

    struct scs_cdt *cdt = NULL;
    enum scs_svc_status st = scs_connect(p->port, &a, &cdt);
    p->last_connect_status = st;
    if (st != SCS_SVC_OK) {
        /* p. 2-47: no open circuit, no CDT, or the port refused. Nothing went
         * out and nothing is pretended to have. The node's cadence is still
         * stamped so a permanently unreachable node does not spin. */
        p->connect_refused++;
        memcpy(p->last_refused_node, nd->addr, 6);
        nd->last_poll_ms = now_ms;
        nd->polled_once = 1;
        p->last_cycle_ms = now_ms;
        p->next_scan = (unsigned)(chosen + 1) % SCS_POLL_MAX_NODES;
        return;
    }

    p->state = SCS_POLL_CONNECTING;
    p->cur = (unsigned)chosen;
    memcpy(p->cur_node, nd->addr, 6);
    p->cdt = cdt;
    p->pending_count = 0;
    p->cycle_start_ms = now_ms;
    p->last_cycle_ms = now_ms;
    nd->last_poll_ms = now_ms;
    nd->polled_once = 1;
    p->next_scan = (unsigned)(chosen + 1) % SCS_POLL_MAX_NODES;
    p->cycles_started++;
}

/*
 * Close the cycle: invoke DISCONNECT (p. 2-50, "After the Process Poller has
 * received replies to all of its inquires, the Process Poller and Directory
 * Service disconnect from each other") and move to DISCONNECTING.
 *
 * The poller does NOT go straight back to IDLE, because p. 2-26 is explicit
 * that one DISCONNECT closes nothing -- the symmetric answer has to arrive.
 * Resting in DISCONNECTING until the CDT is actually released is what makes
 * that visible instead of assumed. The tick() above retries the release,
 * scs_poll_cdt_released() takes the push from the receive path, and failing
 * both the cycle times out; a poller that declared itself IDLE the instant it
 * called DISCONNECT would be reporting a completed dialogue it never saw
 * finish.
 *
 * TWO EARLY IDLE RETURNS BELOW, and neither of them claims a dialogue.
 */
static void poll_close(struct scs_poller *p, uint64_t now_ms)
{
    struct scs_svc_args a;
    poll_args(p, &a, p->cur_node);
    p->state = SCS_POLL_DISCONNECTING;
    p->cycle_start_ms = now_ms; /* the disconnect wait gets its own window */
    p->pending_count = 0;
    if (p->cdt == NULL) {
        /* DESCRIPTORLESS MODE (scs_svc.h): the port has no CDL, so CONNECT
         * succeeded without allocating anything and there is no descriptor to
         * wait for and none to give back. DISCONNECT is not invoked either --
         * scs_disconnect() refuses a NULL CDT, and a DISCONNECT_REQ carrying no
         * Con.ID pair names no connection. Nothing happened, so nothing is
         * counted: this is neither a closed teardown nor a forced release. */
        p->state = SCS_POLL_IDLE;
        memset(p->cur_node, 0, 6);
        return;
    }
    (void)scs_disconnect(p->port, p->cdt, &a);
    /* ALREADY CLOSED BEFORE WE ASKED. The p. 2-28 VC_LOST sweep drives every
     * connection on a broken circuit straight to CLOSED and releases nothing,
     * so a cycle whose circuit died mid-inquiry arrives here holding a CLOSED
     * descriptor. scs_disconnect() has just refused it (no table row from
     * CLOSED, counted in port->illegal), and the honest thing left to do is
     * give the descriptor back the p. 2-26 way -- NOT to count a teardown that
     * never went out, which is why disconnects_closed is not bumped here. */
    if (scs_svc_close_if_closed(p->port, p->cdt)) {
        p->cdt = NULL;
        p->state = SCS_POLL_IDLE;
        memset(p->cur_node, 0, 6);
    }
}

/*
 * Hand one action the connection state machine asked for to the port driver and
 * account for it exactly as scs_svc.c does. The poller drives its OWN
 * connection's transitions (it is the SYSAP that opened it), so it also owns
 * this accounting; using different counters would hide the poller's frames from
 * the port's emitted/unemitted census.
 */
static void poll_emit_action(struct scs_poller *p, enum scs_conn_action act)
{
    if (act == SCS_CONN_ACT_NONE || p->cdt == NULL) {
        return;
    }
    if (p->emit == NULL) {
        p->port->unemitted++;
        return;
    }
    struct scs_svc_args a;
    poll_args(p, &a, p->cur_node);
    const char *what = NULL;
    int r = p->emit(p->emit_ctx, p->cdt, act, &a, &what);
    if (r == SCS_SVC_EMIT_SENT) {
        p->port->emitted++;
    } else if (r == SCS_SVC_EMIT_NOBUILDER) {
        p->port->unemitted++;
    } else {
        p->port->refused++;
    }
}

unsigned scs_poll_opened(struct scs_poller *p, uint64_t now_ms)
{
    if (p == NULL || p->state != SCS_POLL_CONNECTING) {
        return 0;
    }
    /*
     * p. 2-50: "and the Directory Service accepts the connection". On the
     * SOURCE side of Figure 2-14 that acceptance is RCV_ACCEPT_REQ, and it is
     * what takes the connection to OPEN. Driving it through the SAME table
     * scs_connect()/scs_accept() use is what makes the DISCONNECT below legal
     * later -- a poller that skipped it would sit in CONNECT SENT and its
     * DISCONNECT would be an illegal service the machine refuses.
     */
    if (p->cdt != NULL && scs_conn_fsm_enabled()) {
        struct scs_conn_transition t =
            scs_conn_fsm_step(p->cdt, SCS_CONN_EV_RCV_ACCEPT_REQ);
        if (t.illegal) {
            p->cycles_abandoned++;
            scs_poll_abandon(p);
            return 0;
        }
        /* Figure 2-14's ACCEPT_RSP. OVMX has no builder for it (scs_svc.h), so
         * this normally lands in port->unemitted -- recorded, not pretended. */
        poll_emit_action(p, t.action);
    }
    p->state = SCS_POLL_INQUIRING;
    p->cycle_start_ms = now_ms;
    p->pending_count = 0;

    unsigned sent = 0;
    for (unsigned i = 0; i < SCS_POLL_MAX_NAMES; i++) {
        struct scs_poll_name *n = &p->names[i];
        if (!n->in_use || !name_scopes(n, p->cur_node)) {
            continue;
        }
        if (name_disabled_on(n, p->cur_node)) {
            p->skipped_disabled++;
            continue; /* p. 2-50: already connected on this node */
        }
        if (p->inquire == NULL ||
            p->inquire(p->inquire_ctx, p->cdt, p->cur_node, n->sysap) != 1) {
            continue; /* no builder / refused -- NOT recorded as outstanding */
        }
        name_copy(p->pending[p->pending_count], n->sysap);
        p->pending_count++;
        p->inquiries_sent++;
        sent++;
    }

    if (sent == 0) {
        /* Nothing to wait for. Closing now is the same rule as below, with an
         * empty inquiry set. */
        poll_close(p, now_ms);
    }
    return sent;
}

int scs_poll_answer(struct scs_poller *p, const char *sysap,
                    enum scs_dir_answer answer, uint64_t now_ms)
{
    if (p == NULL || sysap == NULL || p->state != SCS_POLL_INQUIRING) {
        if (p != NULL) {
            p->answers_unsolicited++;
        }
        return 0;
    }

    unsigned hit = SCS_POLL_MAX_NAMES;
    for (unsigned i = 0; i < p->pending_count; i++) {
        if (name_eq(p->pending[i], sysap)) {
            hit = i;
            break;
        }
    }
    if (hit == SCS_POLL_MAX_NAMES) {
        p->answers_unsolicited++;
        return 0;
    }

    switch (answer) {
    case SCS_DIR_ANSWER_YES:     p->answers_yes++; break;
    case SCS_DIR_ANSWER_NO:      p->answers_no++; break;
    case SCS_DIR_ANSWER_UNKNOWN: p->answers_unknown++; break;
    }

    /* p. 2-50: "SYSAP_A is notified about the discovery of SYSAP_X ... However,
     * SYSAP_B is not notified about the absence of SYSAP_W". Only YES. An
     * UNKNOWN result field is not an answer we can read (scs_dir.h), so it also
     * notifies nobody. */
    if (answer == SCS_DIR_ANSWER_YES) {
        struct scs_poll_name *n = name_find(p, sysap);
        if (n != NULL && n->on_found != NULL) {
            p->notifications++;
            n->on_found(n->sysap, p->cur_node, n->ctx);
        }
    }

    /* Drop the answered inquiry. */
    for (unsigned i = hit; i + 1 < p->pending_count; i++) {
        memcpy(p->pending[i], p->pending[i + 1], SCS_DIR_NAME_LEN + 1);
    }
    p->pending_count--;

    if (p->pending_count == 0) {
        p->cycles_completed++;
        poll_close(p, now_ms);
    }
    return 1;
}
