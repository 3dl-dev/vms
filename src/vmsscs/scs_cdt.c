/*
 * scs_cdt.c - Connection Descriptor Tables, the Connection Descriptor List and
 * the per-Path-Block connection queue (vms-e1a).
 *
 * See scs_cdt.h for the full provenance, the page cites, the OVMX design
 * choices, the WIRE VERDICT (this module is wire-invisible) and the honest
 * statement of what scsd.c does and does not yet route through here.
 */
#include "scs_cdt.h"

#include <string.h>

/* --- small helpers -------------------------------------------------------- */

/* Copy a SYSAP name into a fixed CDT field, truncating at the field width.
 * NULL leaves the field empty. */
static void set_sysap_name(char dst[SCS_CDT_SYSAP_NAME_LEN + 1], const char *src)
{
    memset(dst, 0, SCS_CDT_SYSAP_NAME_LEN + 1);
    if (src == NULL) {
        return;
    }
    for (size_t i = 0; i < SCS_CDT_SYSAP_NAME_LEN && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
}

/* p. 2-28: queue the CDT to the Path Block of the circuit that supports it. */
static void pb_queue_insert(struct scs_pb *pb, struct scs_cdt *cdt)
{
    cdt->pb_prev = NULL;
    cdt->pb_next = pb->cdt_head;
    if (pb->cdt_head != NULL) {
        pb->cdt_head->pb_prev = cdt;
    }
    pb->cdt_head = cdt;
}

static void pb_queue_remove(struct scs_pb *pb, struct scs_cdt *cdt)
{
    if (cdt->pb_prev != NULL) {
        cdt->pb_prev->pb_next = cdt->pb_next;
    } else if (pb != NULL && pb->cdt_head == cdt) {
        pb->cdt_head = cdt->pb_next;
    }
    if (cdt->pb_next != NULL) {
        cdt->pb_next->pb_prev = cdt->pb_prev;
    }
    cdt->pb_next = NULL;
    cdt->pb_prev = NULL;
}

/* Fill a fresh CDT sitting in CDL slot `slot`. */
static void cdt_open(struct scs_cdt *cdt, unsigned slot, const char *local_sysap,
                     const char *remote_sysap, struct scs_pb *pb)
{
    memset(cdt, 0, sizeof(*cdt));
    cdt->in_use = 1;
    /* p. 2-28/2-29: the 32-bit CONID identifies this CDT, and its low 16 bits
     * are the CDL index it lives at. */
    cdt->local_conid = SCS_CDT_MAKE_CONID(slot);
    cdt->remote_conid = 0; /* learned during formation (p. 2-28) */
    set_sysap_name(cdt->local_sysap, local_sysap);
    set_sysap_name(cdt->remote_sysap, remote_sysap);
    if (pb != NULL) {
        cdt->pb = pb;
        cdt->sb = pb->sb; /* p. 2-28: pointer to the SB for the node at the far end */
        pb_queue_insert(pb, cdt);
    }
}

/* --- lifecycle ------------------------------------------------------------ */

void scs_cdl_init(struct scs_cdl *cdl)
{
    if (cdl == NULL) {
        return;
    }
    memset(cdl, 0, sizeof(*cdl));
    /* "VMS also allocates and marks as unused SCSCONNCNT CDTs. VMS stores the
     * addresses of these CDTs in the first SCSCONNCNT entries of the CDL."
     * (p. 2-30). The remaining 200 entries stay empty until the initial
     * allocation is exhausted. */
    for (unsigned i = 0; i < SCS_CDT_SCSCONNCNT; i++) {
        cdl->entry[i] = &cdl->initial[i];
    }
}

struct scs_cdt *scs_cdl_alloc(struct scs_cdl *cdl, const char *local_sysap,
                              const char *remote_sysap, struct scs_pb *pb)
{
    if (cdl == NULL) {
        return NULL;
    }

    /* Slot 0 is never handed out: it would still yield a nonzero CONID because
     * of the tag, but keeping it reserved means "CDL slot 0" and "CONID not yet
     * known" can never be confused in a log or a dump (OVMX design choice). */
    for (unsigned i = 1; i < SCS_CDT_SCSCONNCNT; i++) {
        struct scs_cdt *cdt = cdl->entry[i];
        if (cdt != NULL && !cdt->in_use) {
            /* Reuse of a released-but-not-deallocated CDT (p. 2-30). */
            cdt_open(cdt, i, local_sysap, remote_sysap, pb);
            return cdt;
        }
    }

    /* "If the initial allocation of CDTs is ever exhausted, VMS will allocate
     * up to 200 more CDTs on an 'as needed' basis, storing their addresses in
     * the 200 extra CDL entries." (p. 2-30) */
    for (unsigned i = SCS_CDT_SCSCONNCNT; i < SCS_CDL_ENTRIES; i++) {
        if (cdl->entry[i] == NULL) {
            struct scs_cdt *cdt = &cdl->overflow[i - SCS_CDT_SCSCONNCNT];
            cdl->entry[i] = cdt;
            cdl->overflow_allocated++;
            cdt_open(cdt, i, local_sysap, remote_sysap, pb);
            return cdt;
        }
        if (!cdl->entry[i]->in_use) {
            cdt_open(cdl->entry[i], i, local_sysap, remote_sysap, pb);
            return cdl->entry[i];
        }
    }

    return NULL; /* CDL full: SCSCONNCNT + 200 connections already open */
}

struct scs_cdt *scs_cdl_alloc_conid(struct scs_cdl *cdl, uint32_t conid,
                                    const char *local_sysap, const char *remote_sysap,
                                    struct scs_pb *pb)
{
    if (cdl == NULL) {
        return NULL;
    }
    if ((conid >> 16) != (uint32_t)SCS_CDT_CONID_TAG) {
        return NULL; /* not a CONID this node could have issued */
    }
    unsigned slot = SCS_CDT_CONID_SLOT(conid);
    if (slot == 0 || slot >= SCS_CDL_ENTRIES) {
        return NULL;
    }

    struct scs_cdt *cdt = cdl->entry[slot];
    if (cdt == NULL) {
        /* An overflow slot not yet populated (p. 2-30 "as needed"). A slot
         * below SCSCONNCNT is always populated by scs_cdl_init, so a NULL there
         * means the CDL was never initialized -- refuse rather than index the
         * overflow array negatively. */
        if (slot < SCS_CDT_SCSCONNCNT) {
            return NULL;
        }
        cdt = &cdl->overflow[slot - SCS_CDT_SCSCONNCNT];
        cdl->entry[slot] = cdt;
        cdl->overflow_allocated++;
    } else if (cdt->in_use) {
        return NULL; /* that CONID is already a live connection */
    }
    cdt_open(cdt, slot, local_sysap, remote_sysap, pb);
    return cdt;
}

void scs_cdl_release(struct scs_cdl *cdl, struct scs_cdt *cdt)
{
    (void)cdl; /* the CDT stays in its CDL slot -- released, not deallocated */
    if (cdt == NULL || !cdt->in_use) {
        return;
    }
    pb_queue_remove(cdt->pb, cdt);
    /* Zero everything but keep the CDT itself in the CDL (p. 2-30). The next
     * scs_cdl_alloc that reuses this slot re-derives local_conid from the slot
     * index, so a stale CONID handed back to us later still fails the
     * local_conid check in scs_cdl_lookup. */
    memset(cdt, 0, sizeof(*cdt));
}

/* --- CDT field setters ---------------------------------------------------- */

void scs_cdt_set_remote_conid(struct scs_cdt *cdt, uint32_t remote_conid)
{
    if (cdt == NULL) {
        return;
    }
    cdt->remote_conid = remote_conid;
}

void scs_cdt_set_handlers(struct scs_cdt *cdt, scs_msg_input_fn msg_input,
                          scs_dgram_input_fn dgram_input, scs_vc_loss_fn vc_loss,
                          void *ctx)
{
    if (cdt == NULL) {
        return;
    }
    cdt->msg_input = msg_input;
    cdt->dgram_input = dgram_input;
    cdt->vc_loss_handler = vc_loss;
    cdt->sysap_ctx = ctx;
}

int scs_cdt_set_connect_data(struct scs_cdt *cdt, const void *data, size_t len)
{
    if (cdt == NULL || len > SCS_CDT_CONNECT_DATA_MAX) {
        return -1;
    }
    if (data == NULL && len != 0) {
        return -1;
    }
    memset(cdt->connect_data, 0, sizeof(cdt->connect_data));
    if (len != 0) {
        memcpy(cdt->connect_data, data, len);
    }
    cdt->connect_data_len = len;
    return 0;
}

void scs_cdt_set_pb(struct scs_cdt *cdt, struct scs_pb *pb)
{
    if (cdt == NULL || cdt->pb == pb) {
        return;
    }
    if (cdt->pb != NULL) {
        pb_queue_remove(cdt->pb, cdt);
    }
    cdt->pb = pb;
    cdt->sb = (pb != NULL) ? pb->sb : NULL;
    if (pb != NULL) {
        pb_queue_insert(pb, cdt);
    }
}

/* --- lookup and delivery -------------------------------------------------- */

struct scs_cdt *scs_cdl_lookup(const struct scs_cdl *cdl, uint32_t conid)
{
    if (cdl == NULL) {
        return NULL;
    }
    /* p. 2-29: the low order 16 bits of the CONID index the CDL. */
    unsigned slot = SCS_CDT_CONID_SLOT(conid);
    if (slot >= SCS_CDL_ENTRIES) {
        return NULL; /* unreachable while SCS_CDL_ENTRIES > 0xFFFF is false, kept explicit */
    }
    struct scs_cdt *cdt = cdl->entry[slot];
    if (cdt == NULL || !cdt->in_use) {
        return NULL;
    }
    /* The whole 32-bit CONID must match, not just the index: a CONID whose slot
     * has been released and reused must not resolve to the new connection. */
    if (cdt->local_conid != conid) {
        return NULL;
    }
    return cdt;
}

/* Shared body of the two delivery entry points (p. 2-29: same CDL lookup, same
 * source-CONID check, different fixed location in the CDT for the routine). */
static int cdl_deliver(struct scs_cdl *cdl, uint32_t dest_conid, uint32_t src_conid,
                       const void *buf, size_t len, int is_datagram)
{
    struct scs_cdt *cdt = scs_cdl_lookup(cdl, dest_conid);
    if (cdt == NULL) {
        return SCS_DELIVER_NO_CDT;
    }
    /* p. 2-35: the packet's source CONID is the sender's local CONID, which is
     * exactly what we recorded as this connection's remote CONID. If both are
     * known and they disagree, the packet is not for this connection. */
    if (src_conid != 0 && cdt->remote_conid != 0 && src_conid != cdt->remote_conid) {
        return SCS_DELIVER_SRC_MISMATCH;
    }
    if (is_datagram) {
        if (cdt->dgram_input == NULL) {
            return SCS_DELIVER_NO_ROUTINE;
        }
        cdt->dgram_input(cdt, buf, len, cdt->sysap_ctx);
    } else {
        if (cdt->msg_input == NULL) {
            return SCS_DELIVER_NO_ROUTINE;
        }
        cdt->msg_input(cdt, buf, len, cdt->sysap_ctx);
    }
    return SCS_DELIVER_OK;
}

int scs_cdl_deliver_message(struct scs_cdl *cdl, uint32_t dest_conid, uint32_t src_conid,
                            const void *buf, size_t len)
{
    return cdl_deliver(cdl, dest_conid, src_conid, buf, len, 0);
}

int scs_cdl_deliver_datagram(struct scs_cdl *cdl, uint32_t dest_conid, uint32_t src_conid,
                             const void *buf, size_t len)
{
    return cdl_deliver(cdl, dest_conid, src_conid, buf, len, 1);
}

/* --- the Path Block connection queue (p. 2-28) ---------------------------- */

struct scs_cdt *scs_pb_first_cdt(const struct scs_pb *pb)
{
    return (pb != NULL) ? pb->cdt_head : NULL;
}

struct scs_cdt *scs_cdt_next_on_pb(const struct scs_cdt *cdt)
{
    return (cdt != NULL) ? cdt->pb_next : NULL;
}

unsigned scs_pb_cdt_count(const struct scs_pb *pb)
{
    unsigned n = 0;
    for (const struct scs_cdt *c = scs_pb_first_cdt(pb); c != NULL; c = c->pb_next) {
        n++;
    }
    return n;
}

unsigned scs_cdl_vc_loss(struct scs_pb *pb)
{
    unsigned n = 0;
    struct scs_cdt *c = scs_pb_first_cdt(pb);
    while (c != NULL) {
        /* Read the link BEFORE the handler runs: a SYSAP error handler is
         * entitled to release its own connection, which unlinks this CDT. */
        struct scs_cdt *next = c->pb_next;
        n++;
        if (c->vc_loss_handler != NULL) {
            c->vc_loss_handler(c, c->sysap_ctx);
        }
        c = next;
    }
    return n;
}

unsigned scs_cdl_in_use_count(const struct scs_cdl *cdl)
{
    unsigned n = 0;
    if (cdl == NULL) {
        return 0;
    }
    for (unsigned i = 0; i < SCS_CDL_ENTRIES; i++) {
        if (cdl->entry[i] != NULL && cdl->entry[i]->in_use) {
            n++;
        }
    }
    return n;
}
