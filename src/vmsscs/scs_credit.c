/*
 * scs_credit.c - SCA message flow control: the Send / Receive / Pending
 * Receive Credit account held per connection in the CDT (vms-76e).
 *
 * See scs_credit.h for the full provenance and page cites, the WIRE VERDICT
 * that grounds the credit field at SCA offset [48:50] from our own captures,
 * the OVMX design choices, the OVMX_NO_CREDIT_ACCOUNTING kill switch, and the
 * blunt statement that scsd.c calls NONE of this and OVMX therefore still puts
 * no live credit on the wire.
 */
#include "scs_credit.h"

#include <stdlib.h>
#include <string.h>

/* --- kill switch ---------------------------------------------------------- */

/* -1 = not yet read; 0 = disabled by OVMX_NO_CREDIT_ACCOUNTING=1; 1 = enabled. */
static int g_credit_enabled = -1;

/* vms-1d2's own switch: -1 = not yet read; 0 = disabled by
 * OVMX_NO_CREDIT_WAIT=1; 1 = enabled. */
static int g_credit_wait_enabled = -1;

void scs_credit_reset_switch_cache(void)
{
    g_credit_enabled = -1;
    g_credit_wait_enabled = -1;
}

int scs_credit_enabled(void)
{
    if (g_credit_enabled < 0) {
        const char *v = getenv("OVMX_NO_CREDIT_ACCOUNTING");
        g_credit_enabled = (v != NULL && v[0] == '1' && v[1] == '\0') ? 0 : 1;
    }
    return g_credit_enabled;
}

int scs_credit_wait_enabled(void)
{
    if (g_credit_wait_enabled < 0) {
        const char *v = getenv("OVMX_NO_CREDIT_WAIT");
        g_credit_wait_enabled = (v != NULL && v[0] == '1' && v[1] == '\0') ? 0 : 1;
    }
    /* No account -> nothing to wait on. OVMX_NO_CREDIT_ACCOUNTING subsumes it. */
    return g_credit_wait_enabled && scs_credit_enabled();
}

/* --- MFREEQ (p. 2-45) ----------------------------------------------------- */

/* The port that supports this connection, or NULL if the CDT is not bound to a
 * Path Block yet. p. 2-43: the message buffers go into "the MFREEQ associated
 * with the port that supports the connection". */
static struct scs_pdt *cdt_port(const struct scs_cdt *cdt)
{
    if (cdt == NULL || cdt->pb == NULL) {
        return NULL;
    }
    return cdt->pb->pdt;
}

static void mfreeq_add(struct scs_cdt *cdt, unsigned n)
{
    struct scs_pdt *pdt = cdt_port(cdt);
    if (pdt != NULL) {
        pdt->mfreeq_count += n;
    }
}

/* A received message is stored in a buffer taken off the MFREEQ; the SYSAP
 * returns it when it releases the buffer. Never underflows. */
static void mfreeq_take(struct scs_cdt *cdt)
{
    struct scs_pdt *pdt = cdt_port(cdt);
    if (pdt != NULL && pdt->mfreeq_count > 0) {
        pdt->mfreeq_count--;
    }
}

static void mfreeq_return(struct scs_cdt *cdt)
{
    mfreeq_add(cdt, 1);
}

/* --- connection formation (p. 2-43) --------------------------------------- */

int scs_credit_extend(struct scs_cdt *cdt, unsigned n, unsigned remote_min_send_credits)
{
    if (cdt == NULL) {
        return -1;
    }
    if (!scs_credit_enabled()) {
        return 0;
    }

    /* p. 2-43: "the local SYSAP requests SCS to allocate a certain number of
     * buffers to receive incoming messages from the remote SYSAP. These buffers
     * are inserted into the Message Free Queue (MFREEQ) associated with the port
     * that supports the connection ... then the local SYSAP is said to have
     * extended [n] Send Credits to the remote SYSAP." */
    cdt->extended_credits = n;
    mfreeq_add(cdt, n);

    /* p. 2-44: "When the connection is formed and 10 Send Credits are extended
     * to the remote SYSAP, local SCS sets the local Receive Credit count to 10
     * for that connection." */
    cdt->receive_credit = n;
    cdt->pending_receive_credit = 0;

    cdt->remote_min_send_credits = remote_min_send_credits;
    return 0;
}

int scs_credit_grant_from_peer(struct scs_cdt *cdt, unsigned n)
{
    if (cdt == NULL) {
        return -1;
    }
    if (!scs_credit_enabled()) {
        return 0;
    }
    /* The mirror of scs_credit_extend on the far side: the remote extended n
     * buffers to us, so n is what WE may send (p. 2-43). */
    unsigned before = cdt->send_credit;
    cdt->send_credit = n;

    /* p. 2-45: "Whenever the Send Credit count in a CDT is increased, the CDT's
     * queue of waiting CDRPs is examined." Formation is one of the two places
     * the count rises. */
    if (cdt->send_credit > before) {
        scs_credit_wait_release(cdt);
    }
    return 0;
}

int scs_credit_set_remote_min_send_credits(struct scs_cdt *cdt, unsigned n)
{
    if (cdt == NULL) {
        return -1;
    }
    if (!scs_credit_enabled()) {
        return 0;
    }
    /* p. 2-44: the Minimum Send Credits argument "passed to the CONNECT or
     * ACCEPT service by the remote SYSAP during connection formation". It moves
     * the dangerously-low threshold and nothing else -- no count changes, and
     * no special credit message is emitted here, because p. 2-44 makes that a
     * RECEIVE-path trigger ("Each time the local node receives a message"). */
    cdt->remote_min_send_credits = n;
    return 0;
}

int scs_credit_set_special_emitter(struct scs_cdt *cdt, scs_credit_special_fn fn, void *ctx)
{
    if (cdt == NULL) {
        return -1;
    }
    cdt->special_emit = fn;
    cdt->special_emit_ctx = ctx;
    return 0;
}

/* --- send path (pp. 2-43..2-45) ------------------------------------------- */

int scs_credit_can_send(const struct scs_cdt *cdt)
{
    if (cdt == NULL) {
        return 0;
    }
    if (!scs_credit_enabled()) {
        return 1;
    }
    /* p. 2-45 Credit Wait: "this routine first verifies that at least one Send
     * Credit is available on the connection being used." */
    return cdt->send_credit > 0 ? 1 : 0;
}

/* p. 2-44: hand the Pending Receive Credit count to the peer and reset it, and
 * raise the local Receive Credit by the same amount because that count mirrors
 * the Send Credit count the remote is about to recompute. Shared by the
 * piggyback path and the special-credit-message path. */
static unsigned take_pending_receive_credit(struct scs_cdt *cdt)
{
    unsigned granted = cdt->pending_receive_credit;
    cdt->pending_receive_credit = 0;
    cdt->receive_credit += granted;
    return granted;
}

int scs_credit_on_send(struct scs_cdt *cdt)
{
    if (cdt == NULL) {
        return -1;
    }
    if (!scs_credit_enabled()) {
        return 0;
    }
    if (cdt->send_credit == 0) {
        /* p. 2-45: the operation belongs in a Credit Wait. OVMX has no CDRP to
         * queue, so it REFUSES rather than pretending the send happened -- a
         * silent success here would be exactly the facade CLAUDE.md rule 9
         * forbids. Nothing is modified. */
        return -1;
    }

    /* p. 2-43: "remote SCS decrements its Send Credit count and sends the
     * message." */
    cdt->send_credit--;

    /* p. 2-44: "local SCS copies the local Pending Receive Credit count, 3, into
     * the credit field of the message header. Local SCS also resets to 0 the
     * local Pending Receive Credit." */
    return (int)take_pending_receive_credit(cdt);
}

/* --- Credit Wait: the queue on the CDT (pp. 2-45..2-46, vms-1d2) ---------- */

/* FIFO append (p. 2-46: "The longer a CDRP has been in the queue, the closer it
 * is to the head of that queue"). */
static void credit_wait_enqueue(struct scs_cdt *cdt, struct scs_credit_waiter *w)
{
    w->cdt = cdt;
    w->next = NULL;
    w->queued = 1;
    w->resumed = 0;
    w->credit = 0;

    if (cdt->credit_wait_tail != NULL) {
        cdt->credit_wait_tail->next = w;
    } else {
        cdt->credit_wait_head = w;
    }
    cdt->credit_wait_tail = w;
    cdt->credit_wait_depth++;
}

/* Pop the head, or NULL. */
static struct scs_credit_waiter *credit_wait_pop(struct scs_cdt *cdt)
{
    struct scs_credit_waiter *w = cdt->credit_wait_head;
    if (w == NULL) {
        return NULL;
    }
    cdt->credit_wait_head = w->next;
    if (cdt->credit_wait_head == NULL) {
        cdt->credit_wait_tail = NULL;
    }
    cdt->credit_wait_depth--;
    w->next = NULL;
    w->queued = 0;
    return w;
}

int scs_credit_send_or_wait(struct scs_cdt *cdt, struct scs_credit_waiter *w)
{
    if (cdt == NULL || w == NULL || w->queued) {
        return -1;
    }
    if (!scs_credit_enabled()) {
        /* vms-76e's kill switch: no account at all, so every send goes. */
        return 0;
    }

    /* p. 2-45: "this routine first verifies that at least one Send Credit is
     * available on the connection being used." Fast path -- the caller sends on
     * its own stack and the waiter is never touched.
     *
     * NOTE the ordering against the queue: a sender that arrives while others
     * are already suspended must NOT overtake them (p. 2-46 makes queue
     * position a function of time waited), so credit is taken by the fast path
     * only when the queue is empty. */
    if (cdt->send_credit > 0 && cdt->credit_wait_head == NULL) {
        cdt->send_credit--;
        return (int)take_pending_receive_credit(cdt);
    }

    if (!scs_credit_wait_enabled()) {
        /* OVMX_NO_CREDIT_WAIT: revert to the vms-76e contract exactly -- refuse
         * the send, queue nothing, move nothing. Never a silent success. */
        return -1;
    }

    /* p. 2-45: "temporarily suspends the operation involved by placing it in a
     * Credit Wait ... by queuing the CDRP representing the operation to the CDT
     * for the connection." Nothing is sent and no count moves. */
    credit_wait_enqueue(cdt, w);
    return SCS_CREDIT_WAIT;
}

unsigned scs_credit_wait_depth(const struct scs_cdt *cdt)
{
    return cdt == NULL ? 0u : cdt->credit_wait_depth;
}

unsigned scs_credit_wait_release(struct scs_cdt *cdt)
{
    if (cdt == NULL || !scs_credit_wait_enabled()) {
        return 0;
    }
    if (cdt->credit_wait_draining) {
        /* OVMX reentrancy guard (see scs_cdt.h): a resumed waiter that receives
         * on this same connection must not re-drain the queue underneath us. */
        return 0;
    }

    /* p. 2-45: "as many waiting CDRPs as possible are resumed, based on the
     * number of Send Credits currently available." Fixed up front so that a
     * waiter enqueued BY a resume callback waits for the next grant rather than
     * being served in the same pass. */
    unsigned budget = cdt->send_credit;
    if (budget > cdt->credit_wait_depth) {
        budget = cdt->credit_wait_depth;
    }
    if (budget == 0) {
        return 0;
    }

    cdt->credit_wait_draining = 1;
    unsigned resumed = 0;
    while (resumed < budget && cdt->send_credit > 0 && cdt->credit_wait_head != NULL) {
        struct scs_credit_waiter *w = credit_wait_pop(cdt);

        /* The resumed operation performs exactly the send it was suspended
         * before: one Send Credit debited (p. 2-43) and the outstanding Pending
         * Receive Credit piggybacked and reset (p. 2-44). The head of the queue
         * therefore carries the whole pending grant and later waiters carry 0,
         * which is what a run of scs_credit_on_send() calls would produce. */
        cdt->send_credit--;
        w->credit = take_pending_receive_credit(cdt);
        w->resumed = 1;
        resumed++;

        if (w->resume != NULL) {
            w->resume(w, w->credit, w->ctx);
        }
    }
    cdt->credit_wait_draining = 0;
    return resumed;
}

int scs_credit_wait_cancel(struct scs_cdt *cdt, struct scs_credit_waiter *w)
{
    if (cdt == NULL || w == NULL || !w->queued || w->cdt != cdt) {
        return -1;
    }
    struct scs_credit_waiter *prev = NULL;
    struct scs_credit_waiter *cur = cdt->credit_wait_head;
    while (cur != NULL && cur != w) {
        prev = cur;
        cur = cur->next;
    }
    if (cur == NULL) {
        return -1;
    }
    if (prev == NULL) {
        cdt->credit_wait_head = cur->next;
    } else {
        prev->next = cur->next;
    }
    if (cdt->credit_wait_tail == cur) {
        cdt->credit_wait_tail = prev;
    }
    cdt->credit_wait_depth--;
    cur->next = NULL;
    cur->queued = 0;
    return 0;
}

unsigned scs_credit_wait_flush(struct scs_cdt *cdt)
{
    if (cdt == NULL) {
        return 0;
    }
    unsigned dropped = 0;
    while (credit_wait_pop(cdt) != NULL) {
        dropped++;
    }
    return dropped;
}

/* --- receive path (pp. 2-43..2-44) ---------------------------------------- */

int scs_credit_on_recv(struct scs_cdt *cdt, unsigned credit)
{
    if (cdt == NULL) {
        return -1;
    }
    if (!scs_credit_enabled()) {
        return 0;
    }

    /* p. 2-44: "remote SCS adds the contents of the credit field to its Send
     * Credit count". */
    cdt->send_credit += credit;

    /* p. 2-44: the arriving message consumes one of the message buffers this
     * node contributed for the connection, so the local Receive Credit -- the
     * mirror of the peer's Send Credit -- drops by one. */
    if (cdt->receive_credit > 0) {
        cdt->receive_credit--;
    }
    mfreeq_take(cdt);

    /* p. 2-45: "Whenever the Send Credit count in a CDT is increased, the CDT's
     * queue of waiting CDRPs is examined." An arriving credit field is the
     * ordinary way that happens -- this is the production trigger for the
     * Credit Wait release, not a side call a test makes by hand. */
    if (credit > 0) {
        scs_credit_wait_release(cdt);
    }

    /* p. 2-44: "Each time the local node receives a message on a connection, it
     * checks to see if the local Receive Credit count for the connection is
     * 'dangerously low'. If it is, and if the local Pending Receive Credit count
     * is greater than 0, local SCS immediately sends remote SCS a special credit
     * message containing the local Pending Receive Credit count."
     *
     * AFTER the Credit Wait release, deliberately: if a suspended send was just
     * resumed it has already piggybacked the outstanding Pending Receive Credit
     * (p. 2-44), leaving nothing for a special credit message to carry. The
     * special credit message exists precisely for the case where nothing is
     * going the other way ("what if it is one-way, at least for awhile", p.
     * 2-44), and this ordering is what makes it fire only in that case. */
    if (scs_credit_wait_enabled() && cdt->special_emit != NULL) {
        int special = scs_credit_take_special(cdt);
        if (special > 0) {
            cdt->special_emit(cdt, (unsigned)special, cdt->special_emit_ctx);
        }
    }
    return 0;
}

int scs_credit_release_buffer(struct scs_cdt *cdt)
{
    if (cdt == NULL) {
        return -1;
    }
    if (!scs_credit_enabled()) {
        return 0;
    }
    /* p. 2-43: the SYSAP "has processed the contents of ... those buffers and
     * released them back to local SCS" -- the buffer is available again but the
     * remote does not know it, which is precisely the Pending Receive Credit. */
    cdt->pending_receive_credit++;
    mfreeq_return(cdt);
    return 0;
}

/* --- special credit messages (p. 2-44) ------------------------------------ */

int scs_credit_is_dangerously_low(const struct scs_cdt *cdt)
{
    if (cdt == NULL || !scs_credit_enabled()) {
        return 0;
    }
    /* p. 2-44, the VMS form: "less than the sum of the local SYSGEN parameter
     * SCSFLOWCUSH and the remote value for Minimum Send Credits." */
    return cdt->receive_credit < (unsigned)SCS_CREDIT_SCSFLOWCUSH + cdt->remote_min_send_credits;
}

int scs_credit_take_special(struct scs_cdt *cdt)
{
    if (cdt == NULL || !scs_credit_enabled()) {
        return -1;
    }
    if (!scs_credit_is_dangerously_low(cdt) || cdt->pending_receive_credit == 0) {
        return -1;
    }
    return (int)take_pending_receive_credit(cdt);
}

/* --- the wire field (see the WIRE VERDICT in scs_credit.h) ---------------- */

int scs_credit_header_offset(uint16_t total_sca_len)
{
    /*
     * EXACTLY the seven SCS message length classes our own captures show
     * carrying a small, credit-shaped value at SCA [48:50]. Each one is
     * measured -- see the WIRE VERDICT in scs_credit.h for the per-class
     * observed-value table, the conservation proof (190) and the
     * SYSGEN-tunable match (110). Everything else is refused rather than
     * guessed at: the block-data-transfer classes carry large unrelated values
     * there, the 41-byte 0x48 short does not even reach offset 48, and the
     * 106-byte class is not an SCS message at all (it is the 0x4113 START /
     * config frame -- see the "WHY 106 IS NOT HERE" note in the header).
     *
     * This keys on LENGTH ONLY. The caller is responsible for having already
     * identified the frame as an SCS message; see the header's note on the
     * 0x?b13 marker.
     */
    switch (total_sca_len) {
    case 58:
    case 62:
    case 66:
    case 86:
    case 94:
    case 110:
    case 190:
        return SCS_CREDIT_FIELD_SCA_OFFSET;
    default:
        return -1;
    }
}

int scs_credit_read_header(const uint8_t *sca, size_t sca_len, uint16_t *out)
{
    if (sca == NULL || out == NULL || sca_len > 0xFFFFu) {
        return -1;
    }
    int off = scs_credit_header_offset((uint16_t)sca_len);
    if (off < 0 || sca_len < (size_t)off + SCS_CREDIT_FIELD_LEN) {
        return -1;
    }
    *out = (uint16_t)((uint16_t)sca[off] | ((uint16_t)sca[off + 1] << 8)); /* LE */
    return 0;
}

int scs_credit_stamp_header(uint8_t *sca, size_t sca_len, unsigned credit)
{
    if (sca == NULL || sca_len > 0xFFFFu || credit > 0xFFFFu) {
        return -1;
    }
    int off = scs_credit_header_offset((uint16_t)sca_len);
    if (off < 0 || sca_len < (size_t)off + SCS_CREDIT_FIELD_LEN) {
        return -1;
    }
    if (!scs_credit_enabled()) {
        /* Kill switch: leave the caller's bytes exactly as they were. */
        return 0;
    }
    sca[off] = (uint8_t)(credit & 0xFFu);
    sca[off + 1] = (uint8_t)((credit >> 8) & 0xFFu);
    return 0;
}
