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

/* DNET_NSP_MAX_DATA (the payload bound). Guarded so a consumer that compiles a
 * PRIVATE copy of the codec (libvms's qio_net_op, rd vms-dda) can include
 * dnet_nsp.h by path first, without the NSP include directory on its -I list. */
#ifndef DNET_NSP_MAX_DATA
#include "dnet_nsp.h"
#endif

/* Linkage of every function below. Empty (external) for the vmsdecnet_broker
 * library NETACP links; libvms defines it `static` before including this header
 * and dnet_broker.c, so the client side carries a private copy of the SAME
 * source -- one codec, no second hand-written record format, and no new
 * universal in LIBVMS$SHR's symbol vector (rd vms-dda). */
#ifndef DNET_BROKER_API
#define DNET_BROKER_API
#endif

/* Return codes (distinct namespace; same discipline as the CTERM/NSP codecs). */
#define DNET_BROKER_OK         0
#define DNET_BROKER_ETRUNC   (-1)   /* input too short for the header or body   */
#define DNET_BROKER_EBADLEN  (-2)   /* declared datalen over the payload bound  */
#define DNET_BROKER_EMAGIC   (-3)   /* magic gate failed -- not this record     */
#define DNET_BROKER_EINVAL   (-4)   /* null argument                            */
#define DNET_BROKER_ENOSPACE (-5)   /* output buffer too small                  */
#define DNET_BROKER_ETIMEDOUT (-6)  /* no correlation-matched response arrived   */
#define DNET_BROKER_EIO      (-7)   /* the transport (mailbox) put/get failed     */

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
/* A READ-ONLY query of NETACP's volatile database (IO$_ACPCONTROL on a _NET:
 * channel, rd vms-30e): data = a dnet_netshow request, response data = one
 * dnet_netshow snapshot record. Needs no logical link on the channel. */
#define DNET_BROKER_OP_SHOW    5u
#define DNET_BROKER_OP_MASK    0x00FFu
/* Modifier: RECV completes at once with DNET_BROKER_ST_ENDOFFILE when nothing is
 * buffered (the IO$M_NOW form of IO$_READVBLK) instead of the client waiting. */
#define DNET_BROKER_OPF_NOW    0x8000u

/* Completion statuses carried in dnet_broker_rsp.status. They ARE the VMS SS$_
 * condition values (ssdef.h) -- named here so this pure codec needs no ssdef.h;
 * decnetd and libvms _Static_assert the equality. The DECnet-specific VMS codes
 * (SS$_LINKDISCON, SS$_REJECT, SS$_NOSUCHNODE, SS$_NOLINKS ...) are not yet in
 * OVMX's ssdef.h, so the nearest existing codes are used, LABELLED (rd vms-dda
 * follow-up): a refused/aborted/disconnected link = ABORT, pool full or the
 * requester over its share = EXQUOTA, an unanswered connect = TIMEOUT, a remote
 * access-control rejection = INVLOGIN, an op on a channel with no link =
 * FILNOTACC, NETACP absent/unresponsive = DEVOFFLINE. */
#define DNET_BROKER_ST_NORMAL      1u      /* SS$_NORMAL      */
#define DNET_BROKER_ST_BADPARAM    20u     /* SS$_BADPARAM    */
#define DNET_BROKER_ST_EXQUOTA     28u     /* SS$_EXQUOTA     */
#define DNET_BROKER_ST_ABORT       44u     /* SS$_ABORT       */
#define DNET_BROKER_ST_ILLIOFUNC   244u    /* SS$_ILLIOFUNC   */
#define DNET_BROKER_ST_TIMEOUT     556u    /* SS$_TIMEOUT     */
#define DNET_BROKER_ST_ENDOFFILE   2160u   /* SS$_ENDOFFILE   */
#define DNET_BROKER_ST_INVLOGIN    8348u   /* SS$_INVLOGIN    */
#define DNET_BROKER_ST_NOSUCHDEV   2312u   /* SS$_NOSUCHDEV   */
#define DNET_BROKER_ST_DEVOFFLINE  132u    /* SS$_DEVOFFLINE  */
#define DNET_BROKER_ST_FILNOTACC   172u    /* SS$_FILNOTACC   */
#define DNET_BROKER_ST_BUFFEROVF   1537u   /* SS$_BUFFEROVF   */

/* On-wire header sizes (fixed LE fields; the data buffer follows). */
#define DNET_BROKER_REQ_HDR    24u  /* magic+corr+pid+handle+reply_unit(5x4) + op+datalen(2x2) */
#define DNET_BROKER_RSP_HDR    14u  /* magic+corr+status(3x4) + datalen(2)          */
#define DNET_BROKER_REQ_MAX    (DNET_BROKER_REQ_HDR + DNET_NSP_MAX_DATA)
#define DNET_BROKER_RSP_MAX    (DNET_BROKER_RSP_HDR + DNET_NSP_MAX_DATA)

struct dnet_broker_req {
    uint32_t corr_id;        /* correlation id (echoed in the response)          */
    uint32_t owner_pid;      /* requesting VMS process id (routing + audit)      */
    uint32_t link_handle;    /* the _NET: exec channel / link handle             */
    uint32_t reply_unit;     /* the client's reply-mailbox unit NETACP answers to
                              * (a1-2 mailbox seam: a mailbox read is destructive,
                              * so each $ASSIGN _NET: channel has its OWN reply
                              * mailbox and carries its unit here -- NETACP writes
                              * the response to MBA<reply_unit>:, routing it to the
                              * right waiter; see docs/design-decnet-net-qio-
                              * mailbox-seam.md).                                 */
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
DNET_BROKER_API int dnet_broker_req_encode(const struct dnet_broker_req *r,
                           uint8_t *buf, size_t cap, size_t *outlen);
DNET_BROKER_API int dnet_broker_rsp_encode(const struct dnet_broker_rsp *r,
                           uint8_t *buf, size_t cap, size_t *outlen);

/*
 * Decode a request/response from buf[0..len-1]. BOUNDS-VALIDATED and never reads
 * past buf[len-1]; *out is zeroed first. Returns DNET_BROKER_OK, or
 * DNET_BROKER_ETRUNC / EBADLEN / EMAGIC / EINVAL.
 */
DNET_BROKER_API int dnet_broker_req_decode(const uint8_t *buf, size_t len,
                           struct dnet_broker_req *out);
DNET_BROKER_API int dnet_broker_rsp_decode(const uint8_t *buf, size_t len,
                           struct dnet_broker_rsp *out);

/*
 * dnet_broker_corr_next - issue the next correlation id from a per-caller
 * monotonic counter (*state), skipping 0 (0 is the "no correlation" sentinel
 * dnet_broker_corr_match rejects). Returns the new id and advances *state.
 */
DNET_BROKER_API uint32_t dnet_broker_corr_next(uint32_t *state);

/*
 * dnet_broker_corr_match - the anti-cross-talk gate: 1 iff req_corr == rsp_corr
 * and both are nonzero, else 0. A response whose id does not match the request
 * it is being delivered against is refused (never routed to the wrong link).
 */
DNET_BROKER_API int dnet_broker_corr_match(uint32_t req_corr, uint32_t rsp_corr);

/* ------------------------------------------------------------------------
 * THE CLIENT SIDE (rd vms-dda) -- what a $QIO on a _NET: channel does with these
 * records, written ONCE here so libvms's qio_net_op (over executive mailboxes)
 * and decnetd's host-floor selftest (over an in-process queue) run the SAME
 * marshalling, correlation and wait logic.
 * ------------------------------------------------------------------------ */

/* The transport under one client channel. put: deliver one encoded request
 * record to NETACP (0 = sent). get: take one response record from THIS
 * channel's own reply queue without blocking (1 = got one, 0 = none yet,
 * -1 = transport failure). idle: let time pass before the next get (libvms
 * sleeps briefly; the host selftest pumps its in-process NETACP).
 * reply_polls bounds how many empty gets a request waits for its response
 * before NETACP is declared unresponsive; open_polls is the (longer) bound for
 * an OPEN, which completes only when the remote answers the Connect Initiate. */
struct dnet_broker_io {
    void *ctx;
    int  (*put)(void *ctx, const uint8_t *rec, size_t len);
    int  (*get)(void *ctx, uint8_t *buf, size_t cap, size_t *len);
    void (*idle)(void *ctx);
    unsigned reply_polls;
    unsigned open_polls;
};

/* Per-channel client state: the correlation counter, the routing identity
 * every request carries, and the link handle NETACP issued at OPEN (0 = no
 * link on this channel). `mismatched` counts responses refused by the
 * correlation gate (stale or cross-talk records), for the selftest/audit. */
struct dnet_broker_chan {
    uint32_t corr_state;
    uint32_t owner_pid;
    uint32_t reply_unit;
    uint32_t handle;
    uint32_t mismatched;
};

/*
 * dnet_broker_call - send ONE request and wait for ITS response: encode, put,
 * then get/idle until a response whose correlation id matches arrives. A
 * response that fails to decode or whose id does not match is DROPPED (counted
 * in *mismatched) and the wait continues -- never delivered to this request.
 * Gives up with DNET_BROKER_ETIMEDOUT after max_polls empty gets.
 */
DNET_BROKER_API int dnet_broker_call(const struct dnet_broker_io *io,
                                     const struct dnet_broker_req *req,
                                     struct dnet_broker_rsp *rsp,
                                     unsigned max_polls, uint32_t *mismatched);

/*
 * dnet_broker_xfer - one logical-link operation on a client channel, the body of
 * a _NET: $QIO: op = DNET_BROKER_OP_* (| DNET_BROKER_OPF_NOW for a RECV).
 *   OPEN : in = the NCB text (NODE"user password account"::"object"); on
 *          success the NETACP-issued link handle is kept in bc->handle.
 *   SEND : in = the message; *xfer = bytes accepted.
 *   RECV : out = the next message (*xfer = its length). Without OPF_NOW the
 *          client waits (re-asking NETACP) until a message or a link failure
 *          arrives; with OPF_NOW an empty link completes ENDOFFILE at once.
 *   CLOSE: disconnects; bc->handle is cleared whatever the outcome.
 * Returns the VMS completion status (DNET_BROKER_ST_*): NETACP's own status, or
 * FILNOTACC (no link open on the channel), BADPARAM (a buffer over the
 * DNET_NSP_MAX_DATA bound), DEVOFFLINE (NETACP did not answer -- never a fake
 * completion, INV-6).
 */
DNET_BROKER_API uint32_t dnet_broker_xfer(struct dnet_broker_chan *bc,
                                          const struct dnet_broker_io *io,
                                          uint16_t op,
                                          const void *in, size_t inlen,
                                          void *out, size_t outcap,
                                          size_t *xfer);

/*
 * dnet_broker_control - one READ-ONLY NETACP control query on a client channel
 * (the body of a _NET: IO$_ACPCONTROL, rd vms-30e): sends `in` (<=
 * DNET_NSP_MAX_DATA) as DNET_BROKER_OP_SHOW and copies NETACP's response data
 * into `out`. No logical link is needed or touched (bc->handle is left alone).
 * Returns NETACP's status; BUFFEROVF (a success-with-warning, *xfer = outcap) if
 * the answer is longer than `out`; BADPARAM for an over-bound request;
 * DEVOFFLINE when NETACP does not answer -- never a fabricated answer (INV-6).
 */
DNET_BROKER_API uint32_t dnet_broker_control(struct dnet_broker_chan *bc,
                                             const struct dnet_broker_io *io,
                                             const void *in, size_t inlen,
                                             void *out, size_t outcap, size_t *xfer);

#endif /* DNET_BROKER_H */
