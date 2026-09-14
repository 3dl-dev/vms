/*
 * dnet_broker.h - the exec<->NETACP request/response record codec for the
 *                 DECnet _NET: $QIO broker (rd vms-22c, a1-2, transport T1).
 *
 * T1 = a mailbox-backed network ACP. A user process $QIO on _NET: (qio_net_op)
 * marshals a REQUEST record and writes it to NETACP through an executive mailbox
 * (vms_mbx, the SYS$NET/ACP-mailbox shape); NETACP services it against the NSP
 * logical-link engine and writes a RESPONSE record back, which the waiting
 * process matches to its parked $QIO by correlation id. The NSP payload
 * (<= DNET_NSP_MAX_DATA = 1024) rides inside the existing 4096-byte mailbox
 * message, so T1 needs no new ioctl -- it reuses vms_kif_mbx_write/read.
 *
 * SECURITY -- the two guards are baked in from the first slice (INV-6, and the
 * A2/A8 boundary discipline the CTERM/FAL wire parsers already follow):
 *   1. BOUNDS VALIDATION at every decode. A record arriving from the other side
 *      of the mailbox is untrusted: the decoder gates on a per-direction MAGIC,
 *      bounds `datalen` at DNET_NSP_MAX_DATA, and checks the total length before
 *      touching the body, so a malformed record can never over-read or fault the
 *      consumer (the executive or NETACP). *out is zeroed first, so a rejected
 *      record leaves no half-filled state behind.
 *   2. CORRELATION IDS against cross-talk. Each request carries a monotonically
 *      issued correlation id; a response is accepted ONLY if its id equals the
 *      request's and is nonzero (dnet_broker_corr_match), so a stale, duplicate,
 *      or mis-routed reply cannot be delivered to the wrong waiting link.
 *
 * CLEAN-ROOM (Rule 8): this is an OVMX-internal transport record between two
 * OVMX components, not a DEC/DECnet wire format.
 */
#ifndef DNET_BROKER_H
#define DNET_BROKER_H

#include <stddef.h>
#include <stdint.h>

#include "dnet_nsp.h"   /* DNET_NSP_MAX_DATA (the payload bound) */

/* Return codes (distinct namespace; same discipline as the CTERM/NSP codecs). */
#define DNET_BROKER_OK         0
#define DNET_BROKER_ETRUNC   (-1)   /* input too short for the header or body   */
#define DNET_BROKER_EBADLEN  (-2)   /* declared datalen over the payload bound  */
#define DNET_BROKER_EMAGIC   (-3)   /* magic gate failed -- not this record     */
#define DNET_BROKER_EINVAL   (-4)   /* null argument                            */
#define DNET_BROKER_ENOSPACE (-5)   /* output buffer too small                  */

/* Per-direction magics: a request decoded as a response (or vice-versa) is
 * refused, not misread. ("NETQ" / "NETR" as big-endian ASCII, stored LE.) */
#define DNET_BROKER_REQ_MAGIC  0x4E455451u
#define DNET_BROKER_RSP_MAGIC  0x4E455452u

/* Broker operations -- the _NET: $QIO logical-link functions in the broker's own
 * namespace, so this codec carries no dependency on iodef.h (qio_net_op maps
 * IO$_ACCESS/WRITEVBLK/READVBLK/DEACCESS onto these). */
#define DNET_BROKER_OP_OPEN    1u   /* open/accept a logical link (IO$_ACCESS)     */
#define DNET_BROKER_OP_SEND    2u   /* send a task-to-task message (IO$_WRITEVBLK) */
#define DNET_BROKER_OP_RECV    3u   /* receive a message (IO$_READVBLK)            */
#define DNET_BROKER_OP_CLOSE   4u   /* disconnect the link (IO$_DEACCESS)          */

/* On-wire header sizes (fixed LE fields; the data buffer follows). */
#define DNET_BROKER_REQ_HDR    20u  /* magic+corr+pid+handle(4x4) + op+datalen(2x2) */
#define DNET_BROKER_RSP_HDR    14u  /* magic+corr+status(3x4) + datalen(2)          */
#define DNET_BROKER_REQ_MAX    (DNET_BROKER_REQ_HDR + DNET_NSP_MAX_DATA)
#define DNET_BROKER_RSP_MAX    (DNET_BROKER_RSP_HDR + DNET_NSP_MAX_DATA)

struct dnet_broker_req {
    uint32_t corr_id;        /* correlation id (echoed in the response)          */
    uint32_t owner_pid;      /* requesting VMS process id (routing + audit)      */
    uint32_t link_handle;    /* the _NET: exec channel / link handle             */
    uint16_t op;             /* DNET_BROKER_OP_*                                 */
    uint16_t datalen;        /* 0..DNET_NSP_MAX_DATA                             */
    uint8_t  data[DNET_NSP_MAX_DATA];
};

struct dnet_broker_rsp {
    uint32_t corr_id;        /* MUST equal the request's corr_id                 */
    uint32_t status;         /* SS$_* result                                     */
    uint16_t datalen;        /* 0..DNET_NSP_MAX_DATA (payload for OP_RECV)       */
    uint8_t  data[DNET_NSP_MAX_DATA];
};

/*
 * Encode a request/response into buf; writes the total length to *outlen.
 * Returns DNET_BROKER_OK, DNET_BROKER_EINVAL (null), DNET_BROKER_EBADLEN
 * (datalen over DNET_NSP_MAX_DATA), or DNET_BROKER_ENOSPACE.
 */
int dnet_broker_req_encode(const struct dnet_broker_req *r,
                           uint8_t *buf, size_t cap, size_t *outlen);
int dnet_broker_rsp_encode(const struct dnet_broker_rsp *r,
                           uint8_t *buf, size_t cap, size_t *outlen);

/*
 * Decode a request/response from buf[0..len-1]. BOUNDS-VALIDATED and never reads
 * past buf[len-1]; *out is zeroed first. Returns DNET_BROKER_OK, or
 * DNET_BROKER_ETRUNC / EBADLEN / EMAGIC / EINVAL.
 */
int dnet_broker_req_decode(const uint8_t *buf, size_t len,
                           struct dnet_broker_req *out);
int dnet_broker_rsp_decode(const uint8_t *buf, size_t len,
                           struct dnet_broker_rsp *out);

/*
 * dnet_broker_corr_next - issue the next correlation id from a per-caller
 * monotonic counter (*state), skipping 0 (0 is the "no correlation" sentinel
 * dnet_broker_corr_match rejects). Returns the new id and advances *state.
 */
uint32_t dnet_broker_corr_next(uint32_t *state);

/*
 * dnet_broker_corr_match - the anti-cross-talk gate: 1 iff req_corr == rsp_corr
 * and both are nonzero, else 0. A response whose id does not match the request
 * it is being delivered against is refused (never routed to the wrong link).
 */
int dnet_broker_corr_match(uint32_t req_corr, uint32_t rsp_corr);

#endif /* DNET_BROKER_H */
