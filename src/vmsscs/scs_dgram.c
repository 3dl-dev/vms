/*
 * scs_dgram.c - SCA datagram buffer management: the per-port Datagram Free
 * Queue (DFREEQ) and the per-connection buffer count in the CDT (vms-b1d).
 *
 * See scs_dgram.h for the full provenance and page cites, for the DIRECTION
 * note (the account is on RECEIVE -- p. 2-42 states outright that SCA provides
 * no flow control for the datagram service, so there is no send-side debit and
 * none was invented), for the OVMX design choices, and for the blunt statement
 * that no production caller routes a datagram through any of this.
 */
#include "scs_dgram.h"

#include <stdlib.h>

/* --- kill switch ---------------------------------------------------------- */

/* -1 = not yet read; 0 = disabled by OVMX_NO_DGRAM_ACCOUNTING=1; 1 = enabled. */
static int g_dgram_enabled = -1;

void scs_dgram_reset_switch_cache(void)
{
    g_dgram_enabled = -1;
}

int scs_dgram_enabled(void)
{
    if (g_dgram_enabled < 0) {
        const char *v = getenv("OVMX_NO_DGRAM_ACCOUNTING");
        g_dgram_enabled = (v != NULL && v[0] == '1' && v[1] == '\0') ? 0 : 1;
    }
    return g_dgram_enabled;
}

/* --- the DFREEQ, p. 2-43 -------------------------------------------------- */

/* The port that supports this connection, or NULL if the CDT is not bound to a
 * Path Block. p. 2-43: "The datagram buffers allocated for a connection are
 * inserted into the DFREEQ for the port that supports that connection." */
static struct scs_pdt *cdt_port(const struct scs_cdt *cdt)
{
    if (cdt == NULL || cdt->pb == NULL) {
        return NULL;
    }
    return cdt->pb->pdt;
}

static void dfreeq_add(struct scs_cdt *cdt, unsigned n)
{
    struct scs_pdt *pdt = cdt_port(cdt);
    if (pdt != NULL) {
        pdt->dfreeq_count += n;
    }
}

static void dfreeq_sub(struct scs_cdt *cdt, unsigned n)
{
    struct scs_pdt *pdt = cdt_port(cdt);
    if (pdt != NULL) {
        pdt->dfreeq_count = (pdt->dfreeq_count > n) ? pdt->dfreeq_count - n : 0;
    }
}

/* --- buffer allocation (p. 2-42 / p. 2-43) -------------------------------- */

int scs_dgram_extend(struct scs_cdt *cdt, unsigned n)
{
    if (cdt == NULL) {
        return -1;
    }
    if (!scs_dgram_enabled()) {
        return 0;
    }

    /* p. 2-42: "A SYSAP can optionally request SCS to allocate a number of
     * buffers to handle incoming datagrams received on a particular
     * connection ... The number of datagram buffers requested by the SYSAP is
     * stored in the CDT that describes the connection. The buffers themselves
     * are inserted into the Datagram Free Queue (DFREEQ) associated with the
     * port that supports the connection."
     *
     * This is formation, so the count is SET. Any buffers a previous use of
     * this CDT still had on deposit leave the DFREEQ with it -- the bank does
     * not keep an account whose owner has gone (p. 2-43). */
    if (cdt->dgram_buffers > 0) {
        dfreeq_sub(cdt, cdt->dgram_buffers);
    }
    cdt->dgram_buffers = n;
    cdt->dgram_extended = n;
    dfreeq_add(cdt, n);
    return 0;
}

unsigned scs_dgram_add_buffers(struct scs_cdt *cdt, unsigned n)
{
    if (cdt == NULL || !scs_dgram_enabled()) {
        return 0;
    }
    /* p. 2-43: "a SYSAP can request SCS to allocate additional buffers to the
     * DFREEQ ... SCS will appropriately alter the CDT to reflect the change." */
    cdt->dgram_buffers += n;
    cdt->dgram_extended += n;
    dfreeq_add(cdt, n);
    return n;
}

unsigned scs_dgram_remove_buffers(struct scs_cdt *cdt, unsigned n)
{
    if (cdt == NULL || !scs_dgram_enabled()) {
        return 0;
    }
    /* p. 2-43: "A SYSAP can also request SCS to deallocate buffers for a
     * connection from the DFREEQ." Clamped at what this connection has on
     * deposit -- "each person is entitled only to the amount of money that he
     * or she has on deposit in the bank" (p. 2-43). */
    if (n > cdt->dgram_buffers) {
        n = cdt->dgram_buffers;
    }
    cdt->dgram_buffers -= n;
    cdt->dgram_extended = (cdt->dgram_extended > n) ? cdt->dgram_extended - n : 0;
    dfreeq_sub(cdt, n);
    return n;
}

/* --- the receive path (p. 2-42) ------------------------------------------- */

int scs_dgram_port_take(struct scs_pdt *pdt)
{
    if (pdt == NULL) {
        return SCS_DGRAM_NO_CDT;
    }
    if (!scs_dgram_enabled()) {
        return 0;
    }
    /* p. 2-42: "When the port receives a datagram, it dequeues the buffer at
     * the head of DFREEQ" -- and "The possibility exists that the DFREEQ
     * itself is empty when the port receives a datagram. If this is the case,
     * the port merely discards the datagram."
     *
     * The discard is silent on the wire (nothing is sent, nothing is NAKed --
     * datagram delivery is best effort, p. 2-33). It is NOT silent locally:
     * the counter is the whole difference between a facility that drops and a
     * facility that does nothing (INV-6). */
    if (pdt->dfreeq_count == 0) {
        pdt->dfreeq_empty_discards++;
        return SCS_DGRAM_DISCARD_DFREEQ_EMPTY;
    }
    pdt->dfreeq_count--;
    return 0;
}

int scs_dgram_deliver(struct scs_cdt *cdt, const void *buf, size_t len)
{
    if (cdt == NULL) {
        return SCS_DGRAM_NO_CDT;
    }
    if (!scs_dgram_enabled()) {
        /* No account at all: deliver if there is anywhere to deliver to. The
         * switch removes the accounting, not the delivery. */
        if (cdt->dgram_input == NULL) {
            return SCS_DGRAM_NO_ROUTINE;
        }
        cdt->dgram_input(cdt, buf, len, cdt->sysap_ctx);
        return SCS_DGRAM_DELIVERED;
    }

    /* p. 2-42: "If the CDT's count of datagram buffers available to this
     * connection is not greater than 0, SCS discards the datagram by inserting
     * the buffer back into the DFREEQ. This prevents datagrams received on one
     * connection from consuming datagram buffers contributed to the DFREEQ for
     * other connections."
     *
     * The buffer returning to the DFREEQ is the point: the port already took
     * it, and this connection is not entitled to it. */
    if (cdt->dgram_buffers == 0) {
        cdt->dgram_discards_no_quota++;
        dfreeq_add(cdt, 1);
        return SCS_DGRAM_DISCARD_NO_QUOTA;
    }

    /* p. 2-29: the datagram input routine's address comes out of the CDT. If
     * the SYSAP installed none there is nothing to deliver to; the buffer goes
     * back exactly as in the no-quota case, and the connection's count is NOT
     * debited because nothing consumed a buffer. Counted separately from the
     * book's discard -- this one is an OVMX state (no SYSAP is installed
     * anywhere in scsd.c today), not a documented SCA outcome. */
    if (cdt->dgram_input == NULL) {
        dfreeq_add(cdt, 1);
        return SCS_DGRAM_NO_ROUTINE;
    }

    /* p. 2-42: "SCS delivers the datagram to the destination SYSAP. This is
     * done by decrementing the count of datagram buffers available to the
     * connection, and then passing the buffer to the datagram input routine
     * whose address is in the CDT." Decrement FIRST, in that order: a SYSAP
     * that releases the buffer from inside its own input routine must see the
     * debit already applied or the account gains a buffer from nowhere.
     *
     * THAT ORDER IS TESTED, not merely asserted here. It is invisible to an
     * inert SYSAP fake -- both orders leave identical final counts -- so
     * test_scs_dgram.c drives it with a RE-ENTRANT SYSAP
     * (test_input_routine_sees_the_debit_already_applied,
     * test_reentrant_delivery_cannot_spend_the_same_buffer_twice,
     * test_release_from_inside_the_input_routine). Swapping these two
     * statements reds all three; before those tests existed it reded nothing. */
    cdt->dgram_buffers--;
    cdt->dgram_delivered++;
    cdt->dgram_input(cdt, buf, len, cdt->sysap_ctx);
    return SCS_DGRAM_DELIVERED;
}

int scs_dgram_cdl_deliver(struct scs_cdl *cdl, uint32_t dest_conid, uint32_t src_conid,
                          const void *buf, size_t len)
{
    /* p. 2-42: "SCS then locates the CDT associated with the destination CONID
     * in the datagram. (In VMS, SCS uses the low order 16 bits of the
     * destination CONID as an index into the CDL to obtain the address of the
     * CDT.)"
     *
     * Resolved through scs_cdl_resolve() -- the SAME function
     * scs_cdl_deliver_datagram() resolves with, including the p. 2-35
     * source-CONID refusal vms-e1a added. This path does NOT reimplement the
     * resolution, and it must not: two receive paths that resolve differently
     * would be a real defect. Both SCS_DELIVER_NO_CDT and
     * SCS_DELIVER_SRC_MISMATCH collapse to SCS_DGRAM_NO_CDT here because the
     * datagram result enum reports buffer outcomes, and neither of those got as
     * far as a buffer. */
    struct scs_cdt *cdt = NULL;
    if (scs_cdl_resolve(cdl, dest_conid, src_conid, &cdt) != SCS_DELIVER_OK) {
        return SCS_DGRAM_NO_CDT;
    }

    /* The port's half first (p. 2-42), reached through the CDT -- see the
     * ORDERING CAVEAT in scs_dgram.h for what that costs. */
    int taken = scs_dgram_port_take(cdt_port(cdt));
    if (taken == SCS_DGRAM_DISCARD_DFREEQ_EMPTY) {
        return SCS_DGRAM_DISCARD_DFREEQ_EMPTY;
    }

    return scs_dgram_deliver(cdt, buf, len);
}

int scs_dgram_release_buffer(struct scs_cdt *cdt)
{
    if (cdt == NULL) {
        return -1;
    }
    if (!scs_dgram_enabled()) {
        return 0;
    }
    /* p. 2-42: "When a SYSAP has finished processing a received datagram, it
     * can request that SCS return the buffer to the DFREEQ and increment the
     * CDT's count of available datagram buffers". Both, not one. */
    cdt->dgram_buffers++;
    dfreeq_add(cdt, 1);
    return 0;
}

int scs_dgram_deallocate_buffer(struct scs_cdt *cdt)
{
    if (cdt == NULL) {
        return -1;
    }
    if (!scs_dgram_enabled()) {
        return 0;
    }
    /* p. 2-42: "or it can simply request that SCS deallocate the buffer." The
     * buffer is gone: neither the DFREEQ depth nor the connection's available
     * count rises, and the connection's extension shrinks to match, because
     * this is exactly how a connection's deposit shrinks without a
     * deallocate-buffers request. */
    if (cdt->dgram_extended > 0) {
        cdt->dgram_extended--;
    }
    return 0;
}

/* --- visibility (INV-6) --------------------------------------------------- */

unsigned long scs_dgram_delivered(const struct scs_cdt *cdt)
{
    return (cdt != NULL) ? cdt->dgram_delivered : 0ul;
}

unsigned long scs_dgram_discards_no_quota(const struct scs_cdt *cdt)
{
    return (cdt != NULL) ? cdt->dgram_discards_no_quota : 0ul;
}

unsigned long scs_dgram_dfreeq_empty_discards(const struct scs_pdt *pdt)
{
    return (pdt != NULL) ? pdt->dfreeq_empty_discards : 0ul;
}

unsigned scs_dgram_report(const struct scs_cdl *cdl, FILE *out)
{
    if (cdl == NULL || out == NULL) {
        return 0;
    }

    /* One line per DISTINCT port reachable from an in-use CDT (p. 2-43: the
     * DFREEQ belongs to the port, and several connections share it). The CDL is
     * small and the port count smaller still, so this is an O(n*ports) scan
     * with no side table. */
    const struct scs_pdt *seen[SCS_CDL_ENTRIES];
    unsigned nseen = 0;
    unsigned lines = 0;

    for (unsigned i = 0; i < SCS_CDL_ENTRIES; i++) {
        const struct scs_cdt *cdt = cdl->entry[i];
        if (cdt == NULL || !cdt->in_use) {
            continue;
        }
        const struct scs_pdt *pdt = cdt_port(cdt);
        if (pdt == NULL) {
            continue;
        }
        unsigned j = 0;
        while (j < nseen && seen[j] != pdt) {
            j++;
        }
        if (j == nseen) {
            seen[nseen++] = pdt;
            fprintf(out, "  DFREEQ: port#%u depth=%u empty-discards=%lu\n",
                    nseen - 1u, pdt->dfreeq_count, pdt->dfreeq_empty_discards);
        }
    }

    for (unsigned i = 0; i < SCS_CDL_ENTRIES; i++) {
        const struct scs_cdt *cdt = cdl->entry[i];
        if (cdt == NULL || !cdt->in_use) {
            continue;
        }
        fprintf(out, "  DGRAM: conid=0x%08X %s buffers=%u/%u delivered=%lu"
                " discarded-no-quota=%lu\n",
                cdt->local_conid, cdt->local_sysap, cdt->dgram_buffers,
                cdt->dgram_extended, cdt->dgram_delivered, cdt->dgram_discards_no_quota);
        lines++;
    }
    return lines;
}
