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

void scs_credit_reset_switch_cache(void)
{
    g_credit_enabled = -1;
}

int scs_credit_enabled(void)
{
    if (g_credit_enabled < 0) {
        const char *v = getenv("OVMX_NO_CREDIT_ACCOUNTING");
        g_credit_enabled = (v != NULL && v[0] == '1' && v[1] == '\0') ? 0 : 1;
    }
    return g_credit_enabled;
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
    cdt->send_credit = n;
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
