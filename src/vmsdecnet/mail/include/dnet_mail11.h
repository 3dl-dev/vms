/*
 * dnet_mail11.h - DECnet MAIL-11 receiver (Session Control object 27), the
 * protocol a VMS `MAIL> SEND` to `NODE::USER` speaks (rd vms-47fd, epic
 * vms-30e).
 *
 * LAYER BOUNDARY. A PURE byte/state library: no socket, no mailbox, no file,
 * no clock, no allocation. Every MAIL-11 record is the payload of one NSP data
 * segment; the server process (MAIL_SERVER.EXE, tools/mail_server.c) hands each
 * received segment to dnet_m11_rx() and ships every reply segment the machine
 * queues. Recipient validation and delivery are CALLBACKS, so the host replay
 * test drives this exact code with the oracle's segments and a recording store.
 *
 * CLEAN-ROOM PROVENANCE (Rule 8). The record sequence, the reply shapes and
 * every constant below come from the real OpenVMS VAX V7.3 VAX1 -> VAX2 capture
 * tests/lab/captures/decnet-mail11-20261008/ (mail11-wire.txt) -- nothing from
 * VSI/HPE source or binaries:
 *   client                                server
 *   sender name (12 bytes, blank padded)
 *   recipient "SYSTEM"               ->   01 00 00 00            (accepted)
 *   recipient "NOSUCHUSER"           ->   12 81 7E 00            (%MAIL-E-NOSUCHUSR)
 *                                          "%MAIL-E-NOSUCHUSR, no such user
 *                                           NOSUCHUSER at node VAX2"
 *                                          00
 *   00                                    (end of recipients)
 *   To: line, CC: line, Subj: line, body lines (one record each, "" = empty)
 *   00                                    (end of message)
 *                                    ->   one status per ACCEPTED recipient
 * The connect user data the VAX client sends (object 27, 16 bytes) and the 16
 * bytes its MAIL_SERVER confirmed with are carried verbatim; their individual
 * fields are not decoded (no public layout was used), so the server confirms a
 * client whose user data has the observed protocol-version prefix with the
 * observed confirm bytes and REFUSES anything else (LABELLED: one observed
 * option set, honestly bounded -- not a claim to every MAIL-11 dialect).
 *
 * INV-6. A message is acknowledged with 01 00 00 00 only when the deliver
 * callback reported it STORED; anything else answers that recipient with a VMS
 * failure status + its text + 00, and a session that ends before the end-of-
 * message record stores nothing and acknowledges nothing.
 *
 * BOUNDED. Network input: every record, the recipient count, the line count and
 * the body size are capped (DNET_M11_MAX_*). An over-bound MESSAGE is refused
 * per recipient with %SYSTEM-F-EXQUOTA (nothing stored); an over-bound or
 * malformed ENVELOPE record (sender / recipient phase) ends the session.
 */
#ifndef DNET_MAIL11_H
#define DNET_MAIL11_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DNET_MAIL11_OBJECT     27      /* DNA well-known object MAIL          */

#define DNET_M11_MAX_REC       512     /* longest record (one line) accepted   */
#define DNET_M11_MAX_SENDER    64      /* sender record, before blank-trim     */
#define DNET_M11_MAX_RCPT      16      /* recipients per message               */
#define DNET_M11_MAX_LINES     2000    /* body lines                           */
#define DNET_M11_MAX_BODY      (64u * 1024u) /* body bytes                     */
#define DNET_M11_MAX_USER      12      /* a VMS username                       */
#define DNET_M11_MAX_NODE      6       /* a Phase IV node name                 */
#define DNET_M11_MAX_TXQ       64      /* queued reply segments                */
#define DNET_M11_MAX_REPLY     160     /* one reply segment                    */

/* Status longwords (little-endian on the wire). */
#define DNET_M11_STS_SUCCESS   0x00000001u
#define DNET_M11_STS_NOSUCHUSR 0x007E8112u   /* %MAIL-E-NOSUCHUSR (oracle)    */
#define DNET_M11_STS_EXQUOTA   0x0000001Cu   /* %SYSTEM-F-EXQUOTA             */
#define DNET_M11_STS_BADPARAM  0x00000014u   /* %SYSTEM-F-BADPARAM            */

/* Session Control connect user data: what the VAX V7.3 client sent and what
 * its MAIL_SERVER confirmed with (the bytes after the 0x10 count). */
#define DNET_M11_CONN_UDLEN    16
extern const uint8_t dnet_m11_client_userdata[DNET_M11_CONN_UDLEN];
extern const uint8_t dnet_m11_accept_userdata[DNET_M11_CONN_UDLEN];

/*
 * dnet_m11_connect_accept - decode the Session Control CONNECT message carried
 * in an inbound object-27 Connect Initiate (`conn`, bounded walk: DSTNAME,
 * SRCNAME, MENUVER, the access-control strings, USRDATA), check the client's
 * user data, and build the Connect Confirm data (count byte + 16 bytes) in
 * `out`. Returns 0 (confirm with *outlen bytes), or -1: not object 27, a
 * malformed message, or a user-data shape this server does not speak -- the
 * caller REFUSES the connect.
 */
int dnet_m11_connect_accept(const uint8_t *conn, size_t len,
                            uint8_t *out, size_t cap, size_t *outlen);

/* One accepted message, handed to the deliver callback. Strings are
 * NUL-terminated; body line i is lines[i] (lens[i] bytes, NUL-terminated). */
struct dnet_m11_msg {
    char        from[DNET_M11_MAX_NODE + 2 + DNET_M11_MAX_SENDER + 1]; /* NODE::USER */
    const char *to;
    const char *cc;
    const char *subj;
    unsigned    nlines;
    const char *const *lines;
    const uint16_t    *lens;
};

/*
 * Callbacks. check_rcpt: is `user` (uppercased, 1..12 chars of A-Z0-9$_) a
 * mailbox on this node? Return 1, or 0. deliver: store `m` in `user`'s mail
 * file; return DNET_M11_STS_SUCCESS only when it is stored, else a VMS status
 * with a "%FAC-S-IDENT, text" line in errtext (NUL-terminated).
 */
struct dnet_m11_ops {
    int      (*check_rcpt)(void *ctx, const char *user);
    uint32_t (*deliver)(void *ctx, const char *user, const struct dnet_m11_msg *m,
                        char *errtext, size_t errcap);
    void     *ctx;
};

enum dnet_m11_state {
    DNET_M11_S_SENDER = 0,   /* expecting the sender record                 */
    DNET_M11_S_RCPT,         /* recipients, until a 00 record                */
    DNET_M11_S_TO,
    DNET_M11_S_CC,
    DNET_M11_S_SUBJ,
    DNET_M11_S_BODY,         /* body lines, until a 00 record                */
    DNET_M11_S_DONE,         /* final statuses queued; the client disconnects */
    DNET_M11_S_ABORT         /* protocol error: the session must end          */
};

#define DNET_M11_OK      0
#define DNET_M11_EPROTO  (-1)    /* session must end: nothing stored, no ack */

struct dnet_m11_server {
    enum dnet_m11_state state;
    struct dnet_m11_ops ops;
    char     local_node[DNET_M11_MAX_NODE + 1];  /* this node, for NOSUCHUSR text */
    char     from[DNET_M11_MAX_NODE + 2 + DNET_M11_MAX_SENDER + 1];
    unsigned nrcpt;
    char     rcpt[DNET_M11_MAX_RCPT][DNET_M11_MAX_USER + 1];
    unsigned nseen;                 /* recipient records seen (accepted or not) */
    char     to[DNET_M11_MAX_REC + 1];
    char     cc[DNET_M11_MAX_REC + 1];
    char     subj[DNET_M11_MAX_REC + 1];
    uint32_t refuse;                /* nonzero: message over-bound / malformed */
    unsigned nlines;
    uint32_t bodylen;
    char     body[DNET_M11_MAX_BODY + DNET_M11_MAX_LINES];  /* NUL-separated */
    const char *lineptr[DNET_M11_MAX_LINES];
    uint16_t linelen[DNET_M11_MAX_LINES];
    unsigned delivered;             /* recipients stored (SUCCESS answered)    */
    /* reply queue */
    unsigned txq_head, txq_n;
    uint16_t txq_len[DNET_M11_MAX_TXQ];
    uint8_t  txq[DNET_M11_MAX_TXQ][DNET_M11_MAX_REPLY];
};

/*
 * dnet_m11_init - arm a receiver. `local_node` is this node's name (the
 * "at node X" of a refusal); `remote_node` names the sender's node ("VAX1", or
 * the decimal address when the node database does not know it) and becomes the
 * NODE:: of the stored From:.
 */
void dnet_m11_init(struct dnet_m11_server *s, const char *local_node,
                   const char *remote_node, const struct dnet_m11_ops *ops);

/* Feed one received record (an NSP data segment's payload, possibly empty).
 * Returns DNET_M11_OK or DNET_M11_EPROTO (end the session). */
int dnet_m11_rx(struct dnet_m11_server *s, const uint8_t *rec, size_t len);

/* Pop the next queued reply segment. Returns 1 with it in buf (length in *len), else 0. */
int dnet_m11_tx_pop(struct dnet_m11_server *s, uint8_t *buf, size_t cap, size_t *len);

/* Is the exchange complete (final statuses queued)? */
int dnet_m11_done(const struct dnet_m11_server *s);

#ifdef __cplusplus
}
#endif

#endif /* DNET_MAIL11_H */
