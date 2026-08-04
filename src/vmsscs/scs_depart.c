/*
 * scs_depart.c - ordered Path-Block teardown on peer departure (vms-17f).
 *
 * See scs_depart.h for the provenance, the page cites, the measured basis of the
 * listen timeout and the honest statement of what is an OVMX design choice.
 * This file builds no frame and opens no socket.
 */
#include "scs_depart.h"

#include <stdlib.h>
#include <string.h>

int scs_depart_enabled(void)
{
    /* Read fresh every call (see the header): a cached answer could not be
     * bracketed by a test, and guardrail 23 requires the switch be RUN. */
    const char *v = getenv("OVMX_NO_PEER_DEPART");
    if (v == NULL || v[0] == '\0') {
        return 1;
    }
    if (v[0] == '0' && v[1] == '\0') {
        return 1;
    }
    return 0;
}

uint64_t scs_depart_listen_timeout_ms(void)
{
    const char *v = getenv("OVMX_PEER_LISTEN_TIMEOUT_MS");
    if (v == NULL || v[0] == '\0') {
        return SCS_DEPART_LISTEN_TIMEOUT_DEFAULT_MS;
    }
    uint64_t ms = 0;
    for (const char *p = v; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return SCS_DEPART_LISTEN_TIMEOUT_DEFAULT_MS; /* not a number: ignore it */
        }
        ms = ms * 10u + (uint64_t)(*p - '0');
        if (ms > 0xFFFFFFFFu) {
            return SCS_DEPART_LISTEN_TIMEOUT_DEFAULT_MS; /* absurd: ignore it */
        }
    }
    if (ms == 0) {
        return SCS_DEPART_LISTEN_TIMEOUT_DEFAULT_MS; /* 0 would depart everyone */
    }
    return ms;
}

enum scs_pb_close_result scs_pb_depart(struct scs_cdl *cdl, struct scs_config *cfg,
                                       struct scs_pb *pb, struct scs_depart_stats *stats)
{
    if (stats != NULL) {
        memset(stats, 0, sizeof(*stats));
    }
    if (!scs_depart_enabled()) {
        return SCS_PB_CLOSE_NOTHING; /* THE KILL SWITCH: nothing is torn down */
    }
    if (cfg == NULL || pb == NULL || !pb->in_use) {
        return SCS_PB_CLOSE_NOTHING;
    }

    /* --- 1. p. 2-28: notify every interested SYSAP FIRST, while the CDTs and
     * the Path Block they point at are all still intact. */
    if (stats != NULL) {
        for (const struct scs_cdt *c = scs_pb_first_cdt(pb); c != NULL;
             c = scs_cdt_next_on_pb(c)) {
            if (c->vc_loss_handler != NULL) {
                stats->handlers_notified++;
            }
        }
    }
    /* connections_lost is what the PRODUCTION scan reported, not a count this
     * function took for itself: if the scan is ever removed, the number goes to
     * zero and says so. */
    unsigned scanned = scs_cdl_vc_loss(pb);
    if (stats != NULL) {
        stats->connections_lost = scanned;
    }

    /* --- 2. Release whatever is still queued. Re-read the head every time: a
     * SYSAP error handler is entitled to have released its own CDT in step 1,
     * and any handler may in principle have unlinked more than its own. */
    struct scs_cdt *c;
    while ((c = scs_pb_first_cdt(pb)) != NULL) {
        /* p. 2-45: a Credit Wait is a suspended operation waiting for credit
         * that this connection will now never grant. Abandon the queue rather
         * than leave the waiters linked to a released CDT. */
        unsigned dropped = scs_credit_wait_flush(c);
        /* Measure what the port ACTUALLY got back rather than what the CDT
         * claimed to owe: scs_cdl_release saturates at 0, and a mismatch
         * between the two is exactly the accounting bug (vms-61b) this reports
         * on. */
        struct scs_pdt *pdt = (c->pb != NULL) ? c->pb->pdt : NULL;
        unsigned before = (pdt != NULL) ? pdt->mfreeq_count : 0;
        scs_cdl_release(cdl, c);
        unsigned after = (pdt != NULL) ? pdt->mfreeq_count : 0;
        if (stats != NULL) {
            stats->waiters_flushed += dropped;
            stats->mfreeq_reclaimed += (before > after) ? (before - after) : 0;
        }
    }

    /* --- 3. Only now is the circuit structure safe to destroy. The System Block
     * deliberately survives (p. 2-17) -- that is the p. 2-21 REFRESH's premise. */
    return scs_pb_close(cfg, pb);
}
