/*
 * dnet_mail11.c - the DECnet MAIL-11 receiver state machine (rd vms-47fd).
 * Read dnet_mail11.h first: provenance, the record sequence, INV-6 and bounds.
 */
#include "dnet_mail11.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* tests/lab/captures/decnet-mail11-20261008/mail11-wire.txt: the RCI's USRDATA
 * (count 0x10) and the CC's accept data (count 0x10). */
const uint8_t dnet_m11_client_userdata[DNET_M11_CONN_UDLEN] = {
    0x03, 0x01, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00 };
const uint8_t dnet_m11_accept_userdata[DNET_M11_CONN_UDLEN] = {
    0x03, 0x01, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00 };

/* The observed protocol-version prefix a client must carry to be confirmed. */
#define M11_VERSION_PREFIX 4

/* ---- Session Control CONNECT walk (bounded) ------------------------------ */

static int skip_counted(const uint8_t *b, size_t len, size_t *p, size_t max)
{
    if (*p >= len) return -1;
    size_t n = b[*p];
    if (n > max || *p + 1 + n > len) return -1;
    *p += 1 + n;
    return 0;
}

/* Walk one end-user descriptor. Returns its object number (format 0) or 0. */
static int skip_name(const uint8_t *b, size_t len, size_t *p, int *object)
{
    if (*p + 2 > len) return -1;
    uint8_t fmt = b[*p], obj = b[*p + 1];
    *p += 2;
    *object = obj;
    switch (fmt) {
    case 0: return 0;
    case 1: return skip_counted(b, len, p, 16);
    case 2:
        if (*p + 4 > len) return -1;
        *p += 4;                                  /* group + user codes */
        return skip_counted(b, len, p, 12);
    default: return -1;
    }
}

int dnet_m11_connect_accept(const uint8_t *conn, size_t len,
                            uint8_t *out, size_t cap, size_t *outlen)
{
    size_t p = 0;
    int dobj = -1, sobj = -1;
    if (!conn || !out || !outlen || cap < 1 + DNET_M11_CONN_UDLEN)
        return -1;
    if (len < 1 || conn[0] != 0) return -1;       /* destination: format 0 */
    if (skip_name(conn, len, &p, &dobj) != 0 || dobj != DNET_MAIL11_OBJECT)
        return -1;
    if (skip_name(conn, len, &p, &sobj) != 0)
        return -1;
    if (p >= len) return -1;
    uint8_t menuver = conn[p++];
    if (menuver & 0x01) {                         /* RQSTRID, PASSWRD, ACCOUNT */
        for (int i = 0; i < 3; i++)
            if (skip_counted(conn, len, &p, 39) != 0) return -1;
    }
    if (!(menuver & 0x02) || p >= len) return -1; /* no USRDATA: not this dialect */
    size_t n = conn[p++];
    if (n != DNET_M11_CONN_UDLEN || p + n != len) return -1;
    if (memcmp(conn + p, dnet_m11_client_userdata, M11_VERSION_PREFIX) != 0)
        return -1;
    out[0] = DNET_M11_CONN_UDLEN;
    memcpy(out + 1, dnet_m11_accept_userdata, DNET_M11_CONN_UDLEN);
    *outlen = 1 + DNET_M11_CONN_UDLEN;
    return 0;
}

/* ---- reply queue ---------------------------------------------------------- */

static int q_push(struct dnet_m11_server *s, const void *b, size_t n)
{
    if (s->txq_n >= DNET_M11_MAX_TXQ || n > DNET_M11_MAX_REPLY) return -1;
    unsigned i = (s->txq_head + s->txq_n) % DNET_M11_MAX_TXQ;
    if (n) memcpy(s->txq[i], b, n);
    s->txq_len[i] = (uint16_t)n;
    s->txq_n++;
    return 0;
}

static int q_status(struct dnet_m11_server *s, uint32_t st)
{
    uint8_t b[4] = { (uint8_t)st, (uint8_t)(st >> 8), (uint8_t)(st >> 16),
                     (uint8_t)(st >> 24) };
    return q_push(s, b, sizeof b);
}

/* A refusal: status longword, its text record, then a 00 record (oracle). */
static int q_refusal(struct dnet_m11_server *s, uint32_t st, const char *text)
{
    static const uint8_t z = 0;
    size_t n = strlen(text);
    if (n > DNET_M11_MAX_REPLY) n = DNET_M11_MAX_REPLY;
    if (q_status(s, st) != 0 || q_push(s, text, n) != 0 || q_push(s, &z, 1) != 0)
        return -1;
    return 0;
}

static const char *bound_text(uint32_t st)
{
    return st == DNET_M11_STS_BADPARAM ? "%SYSTEM-F-BADPARAM, bad parameter value"
                                       : "%SYSTEM-F-EXQUOTA, exceeded quota";
}

/* ---- the machine ---------------------------------------------------------- */

void dnet_m11_init(struct dnet_m11_server *s, const char *local_node,
                   const char *remote_node, const struct dnet_m11_ops *ops)
{
    memset(s, 0, sizeof *s);
    s->state = DNET_M11_S_SENDER;
    if (ops) s->ops = *ops;
    snprintf(s->local_node, sizeof s->local_node, "%s", local_node ? local_node : "");
    snprintf(s->from, sizeof s->from, "%s::", remote_node ? remote_node : "");
}

static int is_end(const uint8_t *rec, size_t len) { return len == 1 && rec[0] == 0; }

/* Printable ASCII/Latin text with no record delimiter inside. */
static int text_ok(const uint8_t *rec, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (rec[i] == 0 || rec[i] == '\n' || rec[i] == '\r') return 0;
    return 1;
}

static void copy_text(char *dst, size_t cap, const uint8_t *rec, size_t len)
{
    if (len >= cap) len = cap - 1;
    memcpy(dst, rec, len);
    dst[len] = '\0';
}

static int rx_sender(struct dnet_m11_server *s, const uint8_t *rec, size_t len)
{
    if (len == 0 || len > DNET_M11_MAX_SENDER) return DNET_M11_EPROTO;
    while (len && rec[len - 1] == ' ') len--;     /* blank-padded to 12 */
    if (len == 0) return DNET_M11_EPROTO;
    for (size_t i = 0; i < len; i++)
        if (rec[i] < 0x20 || rec[i] > 0x7e) return DNET_M11_EPROTO;
    size_t have = strlen(s->from);
    if (have + len >= sizeof s->from) return DNET_M11_EPROTO;
    memcpy(s->from + have, rec, len);
    s->from[have + len] = '\0';
    s->state = DNET_M11_S_RCPT;
    return DNET_M11_OK;
}

static int rx_rcpt(struct dnet_m11_server *s, const uint8_t *rec, size_t len)
{
    if (is_end(rec, len)) { s->state = DNET_M11_S_TO; return DNET_M11_OK; }
    if (len == 0 || len > DNET_M11_MAX_REC) return DNET_M11_EPROTO;
    if (++s->nseen > 4u * DNET_M11_MAX_RCPT) return DNET_M11_EPROTO;

    /* The name as the refusal text echoes it: uppercased, printable, bounded. */
    char shown[40];
    size_t sn = 0;
    int valid = len <= DNET_M11_MAX_USER;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)toupper(rec[i]);
        if (!(isalnum(c) || c == '$' || c == '_')) valid = 0;
        if (sn + 1 < sizeof shown) shown[sn++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    shown[sn] = '\0';

    if (s->nrcpt >= DNET_M11_MAX_RCPT)
        return q_refusal(s, DNET_M11_STS_EXQUOTA, bound_text(DNET_M11_STS_EXQUOTA)) == 0
                   ? DNET_M11_OK : DNET_M11_EPROTO;
    if (valid && s->ops.check_rcpt && s->ops.check_rcpt(s->ops.ctx, shown)) {
        memcpy(s->rcpt[s->nrcpt++], shown, sn + 1);
        return q_status(s, DNET_M11_STS_SUCCESS) == 0 ? DNET_M11_OK : DNET_M11_EPROTO;
    }
    char text[DNET_M11_MAX_REPLY + 1];
    snprintf(text, sizeof text, "%%MAIL-E-NOSUCHUSR, no such user %s at node %s",
             shown, s->local_node);
    return q_refusal(s, DNET_M11_STS_NOSUCHUSR, text) == 0 ? DNET_M11_OK : DNET_M11_EPROTO;
}

static void header_line(struct dnet_m11_server *s, char *dst, size_t cap,
                        const uint8_t *rec, size_t len)
{
    if (len > DNET_M11_MAX_REC) { s->refuse = DNET_M11_STS_EXQUOTA; len = 0; }
    else if (!text_ok(rec, len)) { if (!s->refuse) s->refuse = DNET_M11_STS_BADPARAM; len = 0; }
    copy_text(dst, cap, rec, len);
}

static void body_line(struct dnet_m11_server *s, const uint8_t *rec, size_t len)
{
    if (s->refuse) return;                        /* already refused: drain */
    if (len > DNET_M11_MAX_REC || s->nlines >= DNET_M11_MAX_LINES ||
        s->bodylen + len > DNET_M11_MAX_BODY) {
        s->refuse = DNET_M11_STS_EXQUOTA;
        return;
    }
    if (!text_ok(rec, len)) { s->refuse = DNET_M11_STS_BADPARAM; return; }
    /* bodylen counts payload bytes; the NUL separators live past them. */
    char *d = s->body + s->bodylen + s->nlines;
    memcpy(d, rec, len);
    d[len] = '\0';
    s->lineptr[s->nlines] = d;
    s->linelen[s->nlines] = (uint16_t)len;
    s->nlines++;
    s->bodylen += (uint32_t)len;
}

/* End of message: deliver to every accepted recipient, one status each. */
static int finish(struct dnet_m11_server *s)
{
    struct dnet_m11_msg m;
    memset(&m, 0, sizeof m);
    snprintf(m.from, sizeof m.from, "%s", s->from);
    m.to = s->to; m.cc = s->cc; m.subj = s->subj;
    m.nlines = s->nlines; m.lines = s->lineptr; m.lens = s->linelen;
    for (unsigned i = 0; i < s->nrcpt; i++) {
        if (s->refuse) {
            if (q_refusal(s, s->refuse, bound_text(s->refuse)) != 0) return DNET_M11_EPROTO;
            continue;
        }
        char err[DNET_M11_MAX_REPLY + 1];
        err[0] = '\0';
        uint32_t st = s->ops.deliver
            ? s->ops.deliver(s->ops.ctx, s->rcpt[i], &m, err, sizeof err)
            : DNET_M11_STS_EXQUOTA;
        if (st == DNET_M11_STS_SUCCESS) {
            s->delivered++;
            if (q_status(s, st) != 0) return DNET_M11_EPROTO;
        } else {
            if (st & 1) st = DNET_M11_STS_EXQUOTA;   /* never ack an unstored copy */
            if (!err[0]) snprintf(err, sizeof err, "%s", bound_text(DNET_M11_STS_EXQUOTA));
            if (q_refusal(s, st, err) != 0) return DNET_M11_EPROTO;
        }
    }
    s->state = DNET_M11_S_DONE;
    return DNET_M11_OK;
}

int dnet_m11_rx(struct dnet_m11_server *s, const uint8_t *rec, size_t len)
{
    if (!s || (len && !rec)) return DNET_M11_EPROTO;
    int rc = DNET_M11_OK;
    switch (s->state) {
    case DNET_M11_S_SENDER: rc = rx_sender(s, rec, len); break;
    case DNET_M11_S_RCPT:   rc = rx_rcpt(s, rec, len);   break;
    case DNET_M11_S_TO:
        header_line(s, s->to, sizeof s->to, rec, len);   s->state = DNET_M11_S_CC;   break;
    case DNET_M11_S_CC:
        header_line(s, s->cc, sizeof s->cc, rec, len);   s->state = DNET_M11_S_SUBJ; break;
    case DNET_M11_S_SUBJ:
        header_line(s, s->subj, sizeof s->subj, rec, len); s->state = DNET_M11_S_BODY; break;
    case DNET_M11_S_BODY:
        if (is_end(rec, len)) rc = finish(s);
        else body_line(s, rec, len);
        break;
    case DNET_M11_S_DONE:      /* nothing more is expected; the client disconnects */
    case DNET_M11_S_ABORT:
    default:
        rc = DNET_M11_EPROTO;
        break;
    }
    if (rc != DNET_M11_OK) s->state = DNET_M11_S_ABORT;
    return rc;
}

int dnet_m11_tx_pop(struct dnet_m11_server *s, uint8_t *buf, size_t cap, size_t *len)
{
    if (!s || !s->txq_n) return 0;
    unsigned i = s->txq_head;
    size_t n = s->txq_len[i];
    if (n > cap) return 0;
    if (n) memcpy(buf, s->txq[i], n);
    *len = n;
    s->txq_head = (s->txq_head + 1) % DNET_M11_MAX_TXQ;
    s->txq_n--;
    return 1;
}

int dnet_m11_done(const struct dnet_m11_server *s)
{
    return s && s->state == DNET_M11_S_DONE;
}
