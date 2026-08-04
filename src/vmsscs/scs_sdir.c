/*
 * scs_sdir.c - the queue of SCS Directory Entries and the listening CDTs
 * (vms-7fe). See scs_sdir.h for every page cite, the wire-status grounding and
 * the four labelled OVMX design choices. This file builds no frame and parses
 * no frame except the one grounded 16-byte name field named in the header.
 */
#include "scs_sdir.h"

#include <stdlib.h>
#include <string.h>

const char *scs_sdir_result_name(enum scs_sdir_result r)
{
    switch (r) {
    case SCS_SDIR_DELIVERED:     return "DELIVERED";
    case SCS_SDIR_NO_SUCH_SYSAP: return "NO SUCH SYSAP";
    case SCS_SDIR_BUSY:          return "BUSY";
    case SCS_SDIR_BADARG:        return "BADARG";
    }
    return "?";
}

const char *scs_sdir_state_name(enum scs_sdir_state s)
{
    switch (s) {
    case SCS_SDIR_LISTEN:            return "LISTEN";
    case SCS_SDIR_CONNECT_RECEIVED:  return "CONNECT RECEIVED";
    }
    return "?";
}

int scs_sdir_enabled(void)
{
    const char *v = getenv("OVMX_NO_SDIR");
    if (v == NULL || v[0] == '\0') {
        return 1;
    }
    if (v[0] == '0' && v[1] == '\0') {
        return 1;
    }
    return 0;
}

/*
 * SYSAP names arrive two ways: as a C string from a SYSAP calling LISTEN, and
 * as a 16-byte blank-padded field lifted off the wire. Compare them the same
 * way -- trailing blanks and NULs are not part of the name. A plain loop keeps
 * this free of any POSIX feature-macro dependency (the musl-static build).
 */
static size_t name_len(const char *s)
{
    size_t n = 0;
    while (n < SCS_CDT_SYSAP_NAME_LEN && s[n] != '\0') {
        n++;
    }
    while (n > 0 && s[n - 1] == ' ') {
        n--;
    }
    return n;
}

static int name_eq(const char *a, const char *b)
{
    size_t la = name_len(a);
    size_t lb = name_len(b);
    return la == lb && la > 0 && memcmp(a, b, la) == 0;
}

void scs_sdir_queue_init(struct scs_sdir_queue *q, struct scs_cdl *cdl)
{
    if (q == NULL) {
        return;
    }
    memset(q, 0, sizeof(*q));
    q->cdl = cdl;
}

/*
 * The handler and its context live on the LISTENING CDT, not on the SDIR --
 * p. 2-48: "Instead of containing the address of a regular message input
 * routine, a listening CDT contains the address of the SYSAP's routine for
 * handling incoming connect requests." struct scs_cdt has no connect-request
 * field (it is vms-e1a's, and this item does not widen another module's
 * structure), so OVMX stores the routine in the CDT's msg_input slot through
 * this cast pair and NOWHERE ELSE. That is the whole of p. 2-48's "instead of":
 * the field a normal CDT uses for the message input routine is what a listening
 * CDT uses for the connect-request routine. LABELLED as an OVMX encoding of
 * that sentence, not as a VMS layout.
 *
 * The cast is between two function-pointer types, which C permits so long as
 * the value is only ever called through its original type -- and it is, because
 * nothing delivers a message to a listening CDT: a listening CDT's CONID is
 * never put on the wire (p. 2-49: the local CONID of the new connection is the
 * SEPARATE CDT's), so scs_cdl_deliver_message() can never resolve to one.
 */
union sdir_handler_cast {
    scs_sdir_connect_req_fn on_connect_req;
    scs_msg_input_fn        msg_input;
};

static scs_sdir_connect_req_fn cdt_handler(const struct scs_cdt *cdt)
{
    union sdir_handler_cast u;
    memset(&u, 0, sizeof(u));
    u.msg_input = cdt->msg_input;
    return u.on_connect_req;
}

struct scs_sdir *scs_sdir_listen(struct scs_sdir_queue *q, const char *sysap,
                                 scs_sdir_connect_req_fn on_connect_req, void *ctx)
{
    if (q == NULL || sysap == NULL || sysap[0] == '\0') {
        return NULL;
    }
    /* p. 2-48's queue holds SYSAP NAMEs; two SDIRs for one name would give the
     * scan two answers. LISTEN is not idempotent -- a duplicate is a SYSAP bug
     * worth seeing (the same contract scs_listen() already had). */
    if (scs_sdir_peek(q, sysap) != NULL) {
        return NULL;
    }
    /* No CDL means no listening CDT, and p. 2-48 allocates one per SDIR. There
     * is no degraded "SDIR without a CDT" mode -- fail honestly (INV-6). */
    if (q->cdl == NULL) {
        return NULL;
    }

    for (unsigned i = 0; i < SCS_SDIR_MAX; i++) {
        struct scs_sdir *s = &q->entry[i];
        if (s->in_use) {
            continue;
        }

        /* "Each SDIR contains the CONID of a special 'listening CDT' that is
         * also allocated at this time." (p. 2-48) Claimed at the reserved
         * CONID for this slot -- OVMX DESIGN CHOICE 1. */
        uint32_t conid = SCS_CDT_MAKE_CONID(SCS_SDIR_CONID_SLOT_BASE + i);
        struct scs_cdt *cdt = scs_cdl_alloc_conid(q->cdl, conid, sysap, NULL, NULL);
        if (cdt == NULL) {
            return NULL; /* the reserved slot is occupied -- refuse, do not wander */
        }

        scs_cdt_set_listening(cdt, 1); /* not a connection -- see scs_cdt.h */

        union sdir_handler_cast u;
        memset(&u, 0, sizeof(u));
        u.on_connect_req = on_connect_req;
        scs_cdt_set_handlers(cdt, u.msg_input, NULL, NULL, ctx);

        memset(s, 0, sizeof(*s));
        s->in_use = 1;
        strncpy(s->sysap, sysap, SCS_CDT_SYSAP_NAME_LEN);
        s->sysap[SCS_CDT_SYSAP_NAME_LEN] = '\0';
        s->conid = conid;
        s->state = SCS_SDIR_LISTEN;

        /* Place it in the queue (Figure 2-24), tail first-in-first-out so the
         * walk reports registration order. */
        s->prev = q->tail;
        s->next = NULL;
        if (q->tail != NULL) {
            q->tail->next = s;
        } else {
            q->head = s;
        }
        q->tail = s;

        q->listens++;
        return s;
    }
    return NULL; /* queue full */
}

const struct scs_sdir *scs_sdir_peek(const struct scs_sdir_queue *q, const char *sysap)
{
    if (q == NULL || sysap == NULL) {
        return NULL;
    }
    for (const struct scs_sdir *s = q->head; s != NULL; s = s->next) {
        if (s->in_use && name_eq(s->sysap, sysap)) {
            return s;
        }
    }
    return NULL;
}

const struct scs_sdir *scs_sdir_lookup(struct scs_sdir_queue *q, const char *sysap)
{
    if (q == NULL || sysap == NULL) {
        return NULL;
    }
    q->scans++;
    const struct scs_sdir *s = scs_sdir_peek(q, sysap);
    if (s != NULL) {
        q->hits++;
    }
    return s;
}

struct scs_cdt *scs_sdir_listening_cdt(const struct scs_sdir_queue *q,
                                       const struct scs_sdir *sdir)
{
    if (q == NULL || q->cdl == NULL || sdir == NULL || !sdir->in_use) {
        return NULL;
    }
    return scs_cdl_lookup(q->cdl, sdir->conid);
}

enum scs_sdir_result scs_sdir_connect_req(struct scs_sdir_queue *q,
                                          const char *target_sysap,
                                          const char *remote_sysap,
                                          uint32_t remote_conid,
                                          const struct scs_sdir **sdir_out)
{
    if (sdir_out != NULL) {
        *sdir_out = NULL;
    }
    if (q == NULL || target_sysap == NULL) {
        return SCS_SDIR_BADARG;
    }

    /* "SCS scans this queue looking for an SDIR containing a SYSAP name
     * matching the target name contained in the connect request." (p. 2-48) */
    const struct scs_sdir *found = scs_sdir_lookup(q, target_sysap);
    if (found == NULL) {
        /* "If none is found, SCS replies with a CONNECT_RSP containing the
         * 'no such SYSAP' error." (p. 2-48) */
        q->no_such_sysap++;
        return SCS_SDIR_NO_SUCH_SYSAP;
    }

    struct scs_sdir *s = (struct scs_sdir *)found;

    if (s->state == SCS_SDIR_CONNECT_RECEIVED) {
        if (s->pending_remote_conid != remote_conid) {
            /* "If another connect request is received while the listening CDT
             * is in the CONNECT RECEIVED state, SCS replies with a response
             * that essentially says 'busy ... try again later'." (p. 2-50) */
            q->busy++;
            return SCS_SDIR_BUSY;
        }
        /* Same requester: this is a RETRANSMIT of the request already
         * delivered, not a second SYSAP asking. OVMX DESIGN CHOICE 4. */
        q->retransmits++;
    }

    /* "SCS changes the state of its listening CDT to CONNECT RECEIVED."
     * (p. 2-50) */
    s->state = SCS_SDIR_CONNECT_RECEIVED;
    s->pending_remote_conid = remote_conid;

    /* "SCS uses the CONID in that SDIR to find the target SYSAP's listening
     * CDT. The connect request is then delivered to the routine whose address
     * is in the listening CDT." (p. 2-48) */
    struct scs_cdt *lcdt = scs_sdir_listening_cdt(q, s);
    if (lcdt != NULL) {
        scs_sdir_connect_req_fn fn = cdt_handler(lcdt);
        if (fn != NULL) {
            fn(s->sysap, remote_sysap, remote_conid, lcdt->sysap_ctx);
        }
    }
    q->delivered++;

    if (sdir_out != NULL) {
        *sdir_out = s;
    }
    return SCS_SDIR_DELIVERED;
}

void scs_sdir_connect_answered(struct scs_sdir_queue *q, const struct scs_sdir *sdir)
{
    if (q == NULL || sdir == NULL || !sdir->in_use) {
        return;
    }
    struct scs_sdir *s = (struct scs_sdir *)sdir;
    s->state = SCS_SDIR_LISTEN;
    s->pending_remote_conid = 0;
}

const struct scs_sdir *scs_sdir_first(const struct scs_sdir_queue *q)
{
    return (q != NULL) ? q->head : NULL;
}

const struct scs_sdir *scs_sdir_next(const struct scs_sdir *sdir)
{
    return (sdir != NULL) ? sdir->next : NULL;
}

unsigned scs_sdir_count(const struct scs_sdir_queue *q)
{
    unsigned n = 0;
    if (q == NULL) {
        return 0;
    }
    for (const struct scs_sdir *s = q->head; s != NULL; s = s->next) {
        n++;
    }
    return n;
}

int scs_sdir_target_name(const uint8_t *frame, size_t len,
                         char out[SCS_CDT_SYSAP_NAME_LEN + 1])
{
    if (frame == NULL || out == NULL) {
        return -1;
    }
    /* payload [62:78] == absolute [76:92] (payload byte 0 = abs 14). */
    if (len < 14 + 78) {
        return -1;
    }
    size_t n = 16;
    const uint8_t *f = frame + 14 + 62;
    while (n > 0 && (f[n - 1] == ' ' || f[n - 1] == '\0')) {
        n--;
    }
    if (n > SCS_CDT_SYSAP_NAME_LEN) {
        n = SCS_CDT_SYSAP_NAME_LEN;
    }
    memcpy(out, f, n);
    out[n] = '\0';
    return 0;
}
