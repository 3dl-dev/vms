/*
 * dnet_broker.c - the exec<->NETACP request/response record codec (rd vms-22c,
 *                 a1-2, transport T1). See dnet_broker.h for the design + the two
 *                 security guards (bounds-validated decode; correlation ids).
 *
 * Pure logic: no socket, no mailbox, no allocation. The caller (qio_net_op on
 * the executive side, decnetd on the NETACP side) owns the mailbox I/O and hands
 * bytes to/from these functions. CLEAN-ROOM (Rule 8): an OVMX-internal record.
 */
#ifndef DNET_BROKER_H
#include "dnet_broker.h"
#endif

#include <string.h>

/* Little-endian field helpers (arch-neutral -- the record crosses a process
 * boundary via the mailbox and must decode identically on every substrate). */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}
static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

DNET_BROKER_API int dnet_broker_req_encode(const struct dnet_broker_req *r,
                           uint8_t *buf, size_t cap, size_t *outlen)
{
    if (!r || !buf)
        return DNET_BROKER_EINVAL;
    if (r->datalen > DNET_NSP_MAX_DATA)
        return DNET_BROKER_EBADLEN;

    size_t total = (size_t)DNET_BROKER_REQ_HDR + r->datalen;
    if (cap < total)
        return DNET_BROKER_ENOSPACE;

    put_u32(buf + 0,  DNET_BROKER_REQ_MAGIC);
    put_u32(buf + 4,  r->corr_id);
    put_u32(buf + 8,  r->owner_pid);
    put_u32(buf + 12, r->link_handle);
    put_u32(buf + 16, r->reply_unit);
    put_u16(buf + 20, r->op);
    put_u16(buf + 22, r->datalen);
    if (r->datalen)
        memcpy(buf + DNET_BROKER_REQ_HDR, r->data, r->datalen);

    if (outlen)
        *outlen = total;
    return DNET_BROKER_OK;
}

DNET_BROKER_API int dnet_broker_rsp_encode(const struct dnet_broker_rsp *r,
                           uint8_t *buf, size_t cap, size_t *outlen)
{
    if (!r || !buf)
        return DNET_BROKER_EINVAL;
    if (r->datalen > DNET_NSP_MAX_DATA)
        return DNET_BROKER_EBADLEN;

    size_t total = (size_t)DNET_BROKER_RSP_HDR + r->datalen;
    if (cap < total)
        return DNET_BROKER_ENOSPACE;

    put_u32(buf + 0,  DNET_BROKER_RSP_MAGIC);
    put_u32(buf + 4,  r->corr_id);
    put_u32(buf + 8,  r->status);
    put_u16(buf + 12, r->datalen);
    if (r->datalen)
        memcpy(buf + DNET_BROKER_RSP_HDR, r->data, r->datalen);

    if (outlen)
        *outlen = total;
    return DNET_BROKER_OK;
}

DNET_BROKER_API int dnet_broker_req_decode(const uint8_t *buf, size_t len,
                           struct dnet_broker_req *out)
{
    if (!buf || !out)
        return DNET_BROKER_EINVAL;

    memset(out, 0, sizeof(*out));   /* no half-filled record on any failure */

    if (len < DNET_BROKER_REQ_HDR)
        return DNET_BROKER_ETRUNC;
    if (get_u32(buf + 0) != DNET_BROKER_REQ_MAGIC)
        return DNET_BROKER_EMAGIC;

    uint16_t datalen = get_u16(buf + 22);
    if (datalen > DNET_NSP_MAX_DATA)
        return DNET_BROKER_EBADLEN;          /* over the payload bound */
    if (len < (size_t)DNET_BROKER_REQ_HDR + datalen)
        return DNET_BROKER_ETRUNC;           /* body truncated -- refuse, never over-read */

    out->corr_id     = get_u32(buf + 4);
    out->owner_pid   = get_u32(buf + 8);
    out->link_handle = get_u32(buf + 12);
    out->reply_unit  = get_u32(buf + 16);
    out->op          = get_u16(buf + 20);
    out->datalen     = datalen;
    if (datalen)
        memcpy(out->data, buf + DNET_BROKER_REQ_HDR, datalen);
    return DNET_BROKER_OK;
}

DNET_BROKER_API int dnet_broker_rsp_decode(const uint8_t *buf, size_t len,
                           struct dnet_broker_rsp *out)
{
    if (!buf || !out)
        return DNET_BROKER_EINVAL;

    memset(out, 0, sizeof(*out));

    if (len < DNET_BROKER_RSP_HDR)
        return DNET_BROKER_ETRUNC;
    if (get_u32(buf + 0) != DNET_BROKER_RSP_MAGIC)
        return DNET_BROKER_EMAGIC;

    uint16_t datalen = get_u16(buf + 12);
    if (datalen > DNET_NSP_MAX_DATA)
        return DNET_BROKER_EBADLEN;
    if (len < (size_t)DNET_BROKER_RSP_HDR + datalen)
        return DNET_BROKER_ETRUNC;

    out->corr_id = get_u32(buf + 4);
    out->status  = get_u32(buf + 8);
    out->datalen = datalen;
    if (datalen)
        memcpy(out->data, buf + DNET_BROKER_RSP_HDR, datalen);
    return DNET_BROKER_OK;
}

DNET_BROKER_API uint32_t dnet_broker_corr_next(uint32_t *state)
{
    uint32_t next;
    if (!state)
        return 0;
    next = *state + 1u;
    if (next == 0u)             /* wrapped -- skip the 0 sentinel */
        next = 1u;
    *state = next;
    return next;
}

DNET_BROKER_API int dnet_broker_corr_match(uint32_t req_corr, uint32_t rsp_corr)
{
    return (req_corr != 0u && req_corr == rsp_corr) ? 1 : 0;
}

/* ---------------------------- the client side ---------------------------- */

DNET_BROKER_API int dnet_broker_call(const struct dnet_broker_io *io,
                                     const struct dnet_broker_req *req,
                                     struct dnet_broker_rsp *rsp,
                                     unsigned max_polls, uint32_t *mismatched)
{
    if (!io || !io->put || !io->get || !req || !rsp)
        return DNET_BROKER_EINVAL;

    uint8_t rec[DNET_BROKER_RSP_MAX > DNET_BROKER_REQ_MAX
                ? DNET_BROKER_RSP_MAX : DNET_BROKER_REQ_MAX];
    size_t n = 0;
    int r = dnet_broker_req_encode(req, rec, sizeof rec, &n);
    if (r != DNET_BROKER_OK)
        return r;
    if (io->put(io->ctx, rec, n) != 0)
        return DNET_BROKER_EIO;

    unsigned empty = 0;
    for (;;) {
        size_t got = 0;
        int g = io->get(io->ctx, rec, sizeof rec, &got);
        if (g < 0)
            return DNET_BROKER_EIO;
        if (g == 0) {
            if (++empty > max_polls)
                return DNET_BROKER_ETIMEDOUT;
            if (io->idle)
                io->idle(io->ctx);
            continue;
        }
        /* Untrusted bytes from the other side of the mailbox: bounds-validated
         * decode, then the correlation gate. Anything else is dropped. */
        if (dnet_broker_rsp_decode(rec, got, rsp) != DNET_BROKER_OK ||
            !dnet_broker_corr_match(req->corr_id, rsp->corr_id)) {
            if (mismatched)
                (*mismatched)++;
            continue;
        }
        return DNET_BROKER_OK;
    }
}

DNET_BROKER_API uint32_t dnet_broker_xfer(struct dnet_broker_chan *bc,
                                          const struct dnet_broker_io *io,
                                          uint16_t op,
                                          const void *in, size_t inlen,
                                          void *out, size_t outcap,
                                          size_t *xfer)
{
    struct dnet_broker_req req;   /* per call: concurrent channels never share */
    struct dnet_broker_rsp rsp;
    uint16_t base = (uint16_t)(op & DNET_BROKER_OP_MASK);

    if (xfer)
        *xfer = 0;
    if (!bc || !io)
        return DNET_BROKER_ST_BADPARAM;
    if (base != DNET_BROKER_OP_OPEN && bc->handle == 0)
        return DNET_BROKER_ST_FILNOTACC;          /* no link on this channel */
    if (base == DNET_BROKER_OP_OPEN && bc->handle != 0)
        return DNET_BROKER_ST_BADPARAM;           /* already accessed        */
    if ((base == DNET_BROKER_OP_OPEN || base == DNET_BROKER_OP_SEND) &&
        (inlen > DNET_NSP_MAX_DATA || (inlen && !in)))
        return DNET_BROKER_ST_BADPARAM;           /* over the segment bound  */

    for (;;) {
        memset(&req, 0, sizeof req);
        req.corr_id     = dnet_broker_corr_next(&bc->corr_state);
        req.owner_pid   = bc->owner_pid;
        req.link_handle = bc->handle;
        req.reply_unit  = bc->reply_unit;
        req.op          = op;
        if (base == DNET_BROKER_OP_OPEN || base == DNET_BROKER_OP_SEND) {
            req.datalen = (uint16_t)inlen;
            if (inlen)
                memcpy(req.data, in, inlen);
        }
        int r = dnet_broker_call(io, &req, &rsp,
                                 base == DNET_BROKER_OP_OPEN ? io->open_polls
                                                             : io->reply_polls,
                                 &bc->mismatched);
        if (r != DNET_BROKER_OK) {
            if (base == DNET_BROKER_OP_CLOSE)
                bc->handle = 0;
            return DNET_BROKER_ST_DEVOFFLINE;     /* NETACP did not answer */
        }

        switch (base) {
        case DNET_BROKER_OP_OPEN:
            if ((rsp.status & 1u) && rsp.datalen >= 4) {
                bc->handle = (uint32_t)rsp.data[0] | ((uint32_t)rsp.data[1] << 8) |
                             ((uint32_t)rsp.data[2] << 16) | ((uint32_t)rsp.data[3] << 24);
                if (bc->handle == 0)
                    return DNET_BROKER_ST_ABORT;  /* a zero handle is never valid */
            } else if (rsp.status & 1u) {
                return DNET_BROKER_ST_ABORT;      /* success without a handle: refuse it */
            }
            return rsp.status;
        case DNET_BROKER_OP_SEND:
            if ((rsp.status & 1u) && xfer)
                *xfer = inlen;
            return rsp.status;
        case DNET_BROKER_OP_RECV:
            if (rsp.status == DNET_BROKER_ST_ENDOFFILE && !(op & DNET_BROKER_OPF_NOW)) {
                if (io->idle)
                    io->idle(io->ctx);
                continue;                         /* a waiting read: ask again */
            }
            if (rsp.status & 1u) {
                size_t n = rsp.datalen;
                if (n > outcap || (n && !out))
                    return DNET_BROKER_ST_BADPARAM; /* caller's buffer too small */
                if (n)
                    memcpy(out, rsp.data, n);
                if (xfer)
                    *xfer = n;
            }
            return rsp.status;
        case DNET_BROKER_OP_CLOSE:
        default:
            bc->handle = 0;
            return rsp.status;
        }
    }
}
