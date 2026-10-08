/*
 * dnet_cterm_hostfsm.c - the CTERM HOST role on the real wire (rd vms-a70
 * direction B). See dnet_cterm_hostfsm.h for the ground truth every byte here
 * is checked against (AA-DY88A-TK, AA-DY89A-TK, and the two VAX-host captures).
 *
 * PURE: no socket, no process, no clock, no allocation. Clean-room (Rule 8):
 * written from the public specs and the observed wire only.
 */
#include <string.h>

#include "dnet_cterm_hostfsm.h"

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

/* ---- codec ---------------------------------------------------------------- */

int dnet_cth_bind_request_build(uint8_t *buf, size_t cap, size_t *outlen)
{
    /* MSGTYPE 1 | VERSION 2.4.0 | OPSYS 7 (VMS) | SUPPORT bit 4. A VMS host
     * stops here (REVISION/ID/OPTIONS/NAME absent); sec 4.3 has a receiver
     * ignore what a Bind Request leaves out. */
    static const uint8_t k[8] = { DNET_CTH_F_BIND_REQUEST, 0x02, 0x04, 0x00,
                                  0x07, 0x00, 0x10, 0x00 };
    if (!buf) return DNET_CTH_EINVAL;
    if (cap < sizeof k) return DNET_CTH_ENOSPACE;
    memcpy(buf, k, sizeof k);
    if (outlen) *outlen = sizeof k;
    return DNET_CTH_OK;
}

int dnet_cth_unbind_build(uint16_t reason, uint8_t *buf, size_t cap, size_t *outlen)
{
    if (!buf) return DNET_CTH_EINVAL;
    if (cap < 3) return DNET_CTH_ENOSPACE;
    buf[0] = DNET_CTH_F_UNBIND;
    put16(buf + 1, reason);
    if (outlen) *outlen = 3;
    return DNET_CTH_OK;
}

int dnet_cth_cd_begin(struct dnet_cth_cd *cd, uint8_t *buf, size_t cap)
{
    if (!cd || !buf) return DNET_CTH_EINVAL;
    if (cap < 2) return DNET_CTH_ENOSPACE;
    cd->buf = buf; cd->cap = cap; cd->len = 2;
    buf[0] = DNET_CTH_F_COMMON_DATA;
    buf[1] = 0;                                   /* FILL */
    return DNET_CTH_OK;
}

int dnet_cth_cd_add(struct dnet_cth_cd *cd, const uint8_t *msg, size_t mlen)
{
    if (!cd || !cd->buf || (mlen && !msg) || mlen > 0xffff) return DNET_CTH_EINVAL;
    if (cd->cap - cd->len < 2 + mlen) return DNET_CTH_ENOSPACE;
    put16(cd->buf + cd->len, (uint16_t)mlen);
    if (mlen) memcpy(cd->buf + cd->len + 2, msg, mlen);
    cd->len += 2 + mlen;
    return DNET_CTH_OK;
}

int dnet_cth_cd_iter_init(struct dnet_cth_cd_iter *it, const uint8_t *buf, size_t len)
{
    if (!it || !buf) return DNET_CTH_EINVAL;
    if (len < 2) return DNET_CTH_ETRUNC;
    if (buf[0] != DNET_CTH_F_COMMON_DATA) return DNET_CTH_EINVAL;
    it->buf = buf; it->len = len; it->off = 2;
    return DNET_CTH_OK;
}

int dnet_cth_cd_iter_next(struct dnet_cth_cd_iter *it, const uint8_t **msg, size_t *mlen)
{
    if (!it || !msg || !mlen) return DNET_CTH_EINVAL;
    if (it->off >= it->len) return 0;
    if (it->len - it->off < 2) { it->off = it->len; return DNET_CTH_EBADLEN; }
    size_t l = get16(it->buf + it->off);
    if (l > it->len - it->off - 2) { it->off = it->len; return DNET_CTH_EBADLEN; }
    *msg = it->buf + it->off + 2;
    *mlen = l;
    it->off += 2 + l;
    return 1;
}

int dnet_cth_initiate_build(uint16_t max_msg, uint8_t *buf, size_t cap, size_t *outlen)
{
    /* MSGTYPE 1, FLAGS 0, VERSION 1.4.0, REVISION (8 bytes, implementation-
     * dependent: zero, as both VAX hosts sent), param 1 = max message we
     * accept, param 3 = the supported-message bitmap the VAX hosts sent. */
    uint8_t m[23] = { DNET_CTH_M_INITIATE, 0x00, 0x01, 0x04, 0x00,
                      0, 0, 0, 0, 0, 0, 0, 0,
                      0x01, 0x02, 0x00, 0x00,
                      0x03, 0x04, 0xfe, 0xff, 0xef, 0x00 };
    if (!buf) return DNET_CTH_EINVAL;
    if (cap < sizeof m) return DNET_CTH_ENOSPACE;
    put16(m + 15, max_msg);
    memcpy(buf, m, sizeof m);
    if (outlen) *outlen = sizeof m;
    return DNET_CTH_OK;
}

int dnet_cth_initiate_parse(const uint8_t *msg, size_t len, struct dnet_cth_peer_init *out)
{
    if (!msg || !out) return DNET_CTH_EINVAL;
    memset(out, 0, sizeof *out);
    if (len < 13) return DNET_CTH_ETRUNC;          /* type flags ver(3) rev(8) */
    if (msg[0] != DNET_CTH_M_INITIATE) return DNET_CTH_EINVAL;
    memcpy(out->version, msg + 2, 3);
    size_t off = 13;
    while (off < len) {
        if (len - off < 2) return DNET_CTH_ETRUNC;
        uint8_t type = msg[off], vlen = msg[off + 1];
        if (vlen > len - off - 2) return DNET_CTH_EBADLEN;
        const uint8_t *v = msg + off + 2;
        if (type == 1 && vlen >= 2) out->max_msg = get16(v);
        else if (type == 2 && vlen >= 2) out->input_buf = get16(v);
        else if (type == 3) out->have_bitmap = 1;
        else if (type == DNET_CTH_INIT_P_VMS_TERMCHAR)
            /* A value that does not decode leaves term.valid = 0: the
             * originating terminal then stays honestly unknown. */
            (void)dnet_cth_vms_termchar_parse(v, vlen, &out->term);
        off += 2u + vlen;
    }
    return DNET_CTH_OK;
}

int dnet_cth_vms_termchar_parse(const uint8_t *v, size_t vlen, struct dnet_cth_termchar *out)
{
    if (!v || !out) return DNET_CTH_EINVAL;
    memset(out, 0, sizeof *out);
    if (vlen < 8) return DNET_CTH_ETRUNC;
    if (v[0] != DNET_CTH_DC_TERM) return DNET_CTH_EINVAL;
    out->devclass = v[0];
    out->devtype  = v[1];
    out->width    = get16(v + 2);
    out->ttchar   = (uint32_t)v[4] | ((uint32_t)v[5] << 8) | ((uint32_t)v[6] << 16);
    out->page     = v[7];
    if (vlen >= 12) {
        out->have_tt2 = 1;
        out->tt2char  = (uint32_t)v[8] | ((uint32_t)v[9] << 8) |
                        ((uint32_t)v[10] << 16) | ((uint32_t)v[11] << 24);
    }
    out->valid = 1;
    return DNET_CTH_OK;
}

int dnet_cth_char_build(uint16_t selector, const uint8_t *value, size_t vlen,
                        uint8_t *buf, size_t cap, size_t *outlen)
{
    if (!buf || (vlen && !value)) return DNET_CTH_EINVAL;
    if (cap < 4 + vlen) return DNET_CTH_ENOSPACE;
    buf[0] = DNET_CTH_M_CHARACTERISTICS;
    buf[1] = 0;                                    /* FLAGS = 0 */
    put16(buf + 2, selector);
    if (vlen) memcpy(buf + 4, value, vlen);
    if (outlen) *outlen = 4 + vlen;
    return DNET_CTH_OK;
}

int dnet_cth_write_build(uint16_t flags, uint8_t prefix, uint8_t postfix,
                         const uint8_t *data, size_t dlen,
                         uint8_t *buf, size_t cap, size_t *outlen)
{
    if (!buf || (dlen && !data)) return DNET_CTH_EINVAL;
    if (cap < 5 + dlen) return DNET_CTH_ENOSPACE;
    buf[0] = DNET_CTH_M_WRITE;
    put16(buf + 1, flags);
    buf[3] = prefix;
    buf[4] = postfix;
    if (dlen) memcpy(buf + 5, data, dlen);
    if (outlen) *outlen = 5 + dlen;
    return DNET_CTH_OK;
}

int dnet_cth_start_read_build(const struct dnet_cth_read_req *rq,
                              const uint8_t *prompt, size_t plen,
                              uint8_t *buf, size_t cap, size_t *outlen)
{
    if (!rq || !buf || (plen && !prompt)) return DNET_CTH_EINVAL;
    if (plen > 0xffff) return DNET_CTH_EBADLEN;
    size_t need = 1 + 3 + 12 + 1 + plen;
    if (cap < need) return DNET_CTH_ENOSPACE;
    buf[0] = DNET_CTH_M_START_READ;
    buf[1] = (uint8_t)rq->flags;
    buf[2] = (uint8_t)(rq->flags >> 8);
    buf[3] = (uint8_t)(rq->flags >> 16);
    put16(buf + 4,  rq->max_length);
    put16(buf + 6,  (uint16_t)plen);              /* END-OF-DATA            */
    put16(buf + 8,  (rq->flags & DNET_CTH_RD_TIMED) ? rq->timeout : 0);
    put16(buf + 10, (uint16_t)plen);              /* END-OF-PROMPT          */
    put16(buf + 12, 0);                           /* START-OF-DISPLAY       */
    put16(buf + 14, 0);                           /* LOW-WATER              */
    buf[16] = 0;                                  /* TERMINATION-SET: empty */
    if (plen) memcpy(buf + 17, prompt, plen);
    if (outlen) *outlen = need;
    return DNET_CTH_OK;
}

int dnet_cth_start_read_parse(const uint8_t *msg, size_t len, struct dnet_cth_start_read *out)
{
    if (!msg || !out) return DNET_CTH_EINVAL;
    memset(out, 0, sizeof *out);
    if (len < 17) return DNET_CTH_ETRUNC;
    if (msg[0] != DNET_CTH_M_START_READ) return DNET_CTH_EINVAL;
    out->flags = (uint32_t)msg[1] | ((uint32_t)msg[2] << 8) | ((uint32_t)msg[3] << 16);
    out->max_length       = get16(msg + 4);
    out->end_of_data      = get16(msg + 6);
    out->timeout          = get16(msg + 8);
    out->end_of_prompt    = get16(msg + 10);
    out->start_of_display = get16(msg + 12);
    out->low_water        = get16(msg + 14);
    out->termset_len      = msg[16];
    if (out->termset_len > 32 || out->termset_len > len - 17) return DNET_CTH_EBADLEN;
    out->data = msg + 17 + out->termset_len;
    out->dlen = len - 17 - out->termset_len;
    if (out->end_of_data > out->dlen || out->end_of_prompt > out->end_of_data)
        return DNET_CTH_EBADLEN;
    return DNET_CTH_OK;
}

int dnet_cth_read_data_parse(const uint8_t *msg, size_t len, struct dnet_cth_read_data *out)
{
    if (!msg || !out) return DNET_CTH_EINVAL;
    memset(out, 0, sizeof *out);
    if (len < 8) return DNET_CTH_ETRUNC;
    if (msg[0] != DNET_CTH_M_READ_DATA) return DNET_CTH_EINVAL;
    out->flags     = msg[1];
    out->low_water = get16(msg + 2);
    out->vpos      = msg[4];
    out->hpos      = msg[5];
    out->term_pos  = get16(msg + 6);
    out->data      = msg + 8;
    out->dlen      = len - 8;
    if (out->term_pos > out->dlen) return DNET_CTH_EBADLEN;
    return DNET_CTH_OK;
}

/* ---- FSM ------------------------------------------------------------------ */

void dnet_cth_init(struct dnet_cth *h, unsigned idle_ms)
{
    if (!h) return;
    memset(h, 0, sizeof *h);
    h->state = DNET_CTH_S_IDLE;
    h->seg_max = DNET_CTH_SEG_MAX;
    h->idle_ms = idle_ms ? idle_ms : 1;
}

static unsigned txq_free(const struct dnet_cth *h) { return DNET_CTH_TXQ - h->txcount; }

static int q_seg(struct dnet_cth *h, const uint8_t *seg, size_t len)
{
    if (len > DNET_CTH_SEG_MAX || len > h->seg_max) return DNET_CTH_EBADLEN;
    if (h->txcount >= DNET_CTH_TXQ) return DNET_CTH_ENOSPACE;
    unsigned slot = (h->txhead + h->txcount) % DNET_CTH_TXQ;
    memcpy(h->txq[slot], seg, len);
    h->txlen[slot] = len;
    h->txcount++;
    return DNET_CTH_OK;
}

/* Wrap one CTERM message in a Common Data segment and queue it. */
static int q_cd_one(struct dnet_cth *h, const uint8_t *msg, size_t mlen)
{
    uint8_t seg[DNET_CTH_SEG_MAX];
    struct dnet_cth_cd cd;
    int rc = dnet_cth_cd_begin(&cd, seg, h->seg_max < sizeof seg ? h->seg_max : sizeof seg);
    if (rc == DNET_CTH_OK) rc = dnet_cth_cd_add(&cd, msg, mlen);
    if (rc == DNET_CTH_OK) rc = q_seg(h, seg, cd.len);
    return rc;
}

/* Data bytes one Write can carry inside one segment. */
static size_t write_chunk(const struct dnet_cth *h) { return h->seg_max - 4 - 5; }

static unsigned writes_needed(const struct dnet_cth *h, size_t n)
{
    size_t c = write_chunk(h);
    return (unsigned)((n + c - 1) / c);
}

static int q_write(struct dnet_cth *h, const uint8_t *data, size_t n)
{
    size_t c = write_chunk(h);
    while (n) {
        size_t k = n < c ? n : c;
        uint8_t m[DNET_CTH_SEG_MAX];
        size_t ml = 0;
        int rc = dnet_cth_write_build(DNET_CTH_WR_RAW, 0, 0, data, k, m, sizeof m, &ml);
        if (rc == DNET_CTH_OK) rc = q_cd_one(h, m, ml);
        if (rc != DNET_CTH_OK) return rc;
        h->writes_sent++;
        data += k; n -= k;
    }
    return DNET_CTH_OK;
}

static int q_start_read(struct dnet_cth *h, const uint8_t *prompt, size_t plen, int noecho)
{
    struct dnet_cth_read_req rq;
    memset(&rq, 0, sizeof rq);
    rq.flags = DNET_CTH_RD_FORMAT | DNET_CTH_RD_TERM_ECHO | DNET_CTH_RD_TERMSET_UNIV |
               (noecho ? DNET_CTH_RD_NOECHO : 0);
    /* The buffer holds the prompt and the input; never solicit more than the
     * server said it can buffer (Initiate parameter 2), when it said. */
    size_t ml = plen + DNET_CTH_READ_MAX;
    if (h->peer.input_buf && ml > h->peer.input_buf) ml = h->peer.input_buf;
    rq.max_length = (uint16_t)ml;
    uint8_t m[DNET_CTH_SEG_MAX];
    size_t mlen = 0;
    int rc = dnet_cth_start_read_build(&rq, prompt, plen, m, sizeof m, &mlen);
    if (rc == DNET_CTH_OK) rc = q_cd_one(h, m, mlen);
    if (rc == DNET_CTH_OK) { h->read_active = 1; h->reads_sent++; }
    return rc;
}

int dnet_cth_open(struct dnet_cth *h)
{
    if (!h) return DNET_CTH_EINVAL;
    if (h->state != DNET_CTH_S_IDLE) return DNET_CTH_ESTATE;
    uint8_t m[8];
    size_t ml = 0;
    int rc = dnet_cth_bind_request_build(m, sizeof m, &ml);
    if (rc == DNET_CTH_OK) rc = q_seg(h, m, ml);
    if (rc == DNET_CTH_OK) h->state = DNET_CTH_S_BIND_SENT;
    return rc;
}

/* The two Common Data messages a VMS host sends once Bind Accept arrives. */
static int q_host_negotiation(struct dnet_cth *h)
{
    uint8_t seg[64], m[32];
    size_t ml = 0;
    struct dnet_cth_cd cd;
    static const uint8_t k_input_count_state[2] = { 0x02, 0x00 };
    static const uint8_t k_ctrlc_not_oob[3]     = { 0x03, 0x3b, 0x00 };
    /* We accept a CTERM message as large as one NSP segment carries, less the
     * 4 bytes of Common Data framing: the honest bound, not the VAX's 0x1e10. */
    int rc = dnet_cth_cd_begin(&cd, seg, sizeof seg);
    if (rc == DNET_CTH_OK) rc = dnet_cth_initiate_build((uint16_t)(DNET_CTH_SEG_MAX - 4), m, sizeof m, &ml);
    if (rc == DNET_CTH_OK) rc = dnet_cth_cd_add(&cd, m, ml);
    if (rc == DNET_CTH_OK) rc = dnet_cth_char_build(0x0208, k_input_count_state, 2, m, sizeof m, &ml);
    if (rc == DNET_CTH_OK) rc = dnet_cth_cd_add(&cd, m, ml);
    if (rc == DNET_CTH_OK) rc = q_seg(h, seg, cd.len);
    if (rc == DNET_CTH_OK) rc = dnet_cth_char_build(0x0202, k_ctrlc_not_oob, 3, m, sizeof m, &ml);
    if (rc == DNET_CTH_OK) rc = q_cd_one(h, m, ml);
    return rc;
}

static void in_append(struct dnet_cth *h, const uint8_t *b, size_t n)
{
    size_t room = DNET_CTH_IN_MAX - h->inlen;
    if (n > room) n = room;                       /* bounded: excess dropped */
    memcpy(h->in + h->inlen, b, n);
    h->inlen += n;
}

int dnet_cth_rx(struct dnet_cth *h, const uint8_t *seg, size_t len, uint64_t now_ms)
{
    if (!h || !seg || len == 0) return DNET_CTH_EINVAL;
    if (h->state == DNET_CTH_S_UNBOUND) return DNET_CTH_ESTATE;

    switch (seg[0]) {
    case DNET_CTH_F_BIND_ACCEPT:
        if (h->state != DNET_CTH_S_BIND_SENT) { h->rx_ignored++; return DNET_CTH_OK; }
        /* MSGTYPE + VERSION at least; a major version other than 2 is a
         * binding we do not speak. */
        if (len < 4 || seg[1] != 0x02) return DNET_CTH_EPROTO;
        h->rx_msgs++;
        if (q_host_negotiation(h) != DNET_CTH_OK) return DNET_CTH_EPROTO;
        h->state = DNET_CTH_S_INIT_SENT;
        return DNET_CTH_OK;

    case DNET_CTH_F_UNBIND:
        h->peer_unbound = 1;
        h->peer_unbind_reason = len >= 3 ? get16(seg + 1) : 0;
        h->state = DNET_CTH_S_UNBOUND;
        h->read_active = 0;
        return DNET_CTH_OK;

    case DNET_CTH_F_COMMON_DATA: {
        if (h->state != DNET_CTH_S_INIT_SENT && h->state != DNET_CTH_S_BOUND)
            return DNET_CTH_EPROTO;
        struct dnet_cth_cd_iter it;
        int rc = dnet_cth_cd_iter_init(&it, seg, len);
        if (rc != DNET_CTH_OK) return rc;
        const uint8_t *m; size_t ml;
        while ((rc = dnet_cth_cd_iter_next(&it, &m, &ml)) == 1) {
            if (ml == 0) continue;
            h->rx_msgs++;
            switch (m[0]) {
            case DNET_CTH_M_INITIATE:
                if (h->state != DNET_CTH_S_INIT_SENT) { h->rx_ignored++; break; }
                if (dnet_cth_initiate_parse(m, ml, &h->peer) != DNET_CTH_OK)
                    return DNET_CTH_EPROTO;
                /* Never send a segment larger than the server accepts. Its
                 * floor is the spec's server minimum (139). */
                if (h->peer.max_msg) {
                    if (h->peer.max_msg < 139) return DNET_CTH_EPROTO;
                    if (h->peer.max_msg < h->seg_max) h->seg_max = h->peer.max_msg;
                }
                {
                    /* The VMS message 23 both VAX hosts answered with,
                     * literally (its fields are not in the public spec). */
                    static const uint8_t k_vms23[11] = { DNET_CTH_M_VMS_23, 0x00, 0x00, 0x01,
                                                         0, 0, 0, 0, 0, 0, 0 };
                    if (q_cd_one(h, k_vms23, sizeof k_vms23) != DNET_CTH_OK)
                        return DNET_CTH_EPROTO;
                }
                h->state = DNET_CTH_S_BOUND;
                h->last_event_ms = now_ms;
                break;
            case DNET_CTH_M_READ_DATA: {
                struct dnet_cth_read_data rd;
                if (h->state != DNET_CTH_S_BOUND) { h->rx_ignored++; break; }
                if (dnet_cth_read_data_parse(m, ml, &rd) != DNET_CTH_OK)
                    return DNET_CTH_EBADLEN;
                in_append(h, rd.data, rd.dlen);   /* terminator included */
                h->read_active = 0;
                h->reads_done++;
                h->last_event_ms = now_ms;
                break;
            }
            case DNET_CTH_M_OOB:
                if (h->state != DNET_CTH_S_BOUND || ml < 3) { h->rx_ignored++; break; }
                in_append(h, m + 2, 1);
                break;
            default:
                /* Write Completion, Characteristics replies, Input Count/State,
                 * Discard State, and the VMS-private 15/19/23: nothing this
                 * byte-stream host asked for or acts on. Bounded and dropped. */
                h->rx_ignored++;
                break;
            }
        }
        return rc < 0 ? rc : DNET_CTH_OK;
    }
    default:
        h->rx_ignored++;
        return DNET_CTH_EINVAL;
    }
}

size_t dnet_cth_term_output(struct dnet_cth *h, const uint8_t *bytes, size_t n,
                            uint64_t now_ms)
{
    if (!h || !bytes) return 0;
    size_t took = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = bytes[i];
        if (h->echopos < h->echolen) {
            if (b == h->echo[h->echopos]) {
                took++;
                if (++h->echopos == h->echolen) h->echolen = h->echopos = 0;
                continue;
            }
            h->echolen = h->echopos = 0;          /* not the echo: stop guessing */
        }
        if (h->outlen >= DNET_CTH_OUT_MAX) break;
        h->out[h->outlen++] = b;
        took++;
    }
    if (took) { h->last_out_ms = now_ms; h->last_event_ms = now_ms; }
    return took;
}

static void out_consume(struct dnet_cth *h, size_t k)
{
    memmove(h->out, h->out + k, h->outlen - k);
    h->outlen -= k;
}

int dnet_cth_tick(struct dnet_cth *h, uint64_t now_ms, int term_echo)
{
    if (!h) return DNET_CTH_EINVAL;
    if (h->state != DNET_CTH_S_BOUND) return DNET_CTH_OK;
    int quiet = now_ms - h->last_out_ms >= h->idle_ms;

    if (h->outlen && (quiet || h->outlen >= DNET_CTH_OUT_MAX / 2)) {
        if (h->read_active || !quiet) {
            /* A read is out (VMS breaks a write through it), or output is
             * still streaming: write what we have up to the last line end. */
            size_t k = h->outlen;
            if (!quiet) {
                while (k && h->out[k - 1] != '\n') k--;
                if (!k) k = h->outlen;
            }
            if (txq_free(h) < writes_needed(h, k)) return DNET_CTH_OK;
            if (q_write(h, h->out, k) != DNET_CTH_OK) return DNET_CTH_ENOSPACE;
            out_consume(h, k);
            return DNET_CTH_OK;
        }
        /* Quiet, no read out: lines go as a Write, the unterminated tail is
         * the prompt of the read that solicits the next input -- the way
         * LOGINOUT's "Username: " and DCL's "$ " ride in a Start Read. */
        size_t head = h->outlen;
        while (head && h->out[head - 1] != '\n') head--;
        if (h->outlen - head > DNET_CTH_PROMPT_MAX) head = h->outlen;
        if (txq_free(h) < writes_needed(h, head) + 1) return DNET_CTH_OK;
        if (head && q_write(h, h->out, head) != DNET_CTH_OK) return DNET_CTH_ENOSPACE;
        out_consume(h, head);
        if (q_start_read(h, h->out, h->outlen, !term_echo) != DNET_CTH_OK)
            return DNET_CTH_ENOSPACE;
        h->outlen = 0;
        return DNET_CTH_OK;
    }

    if (!h->outlen && !h->read_active && now_ms - h->last_event_ms >= DNET_CTH_EMPTY_READ_MS &&
        txq_free(h) >= 1)
        return q_start_read(h, NULL, 0, !term_echo);
    return DNET_CTH_OK;
}

size_t dnet_cth_term_input(struct dnet_cth *h, uint8_t *buf, size_t cap, int term_echo)
{
    if (!h || !buf || !cap || !h->inlen) return 0;
    size_t n = h->inlen < cap ? h->inlen : cap;
    memcpy(buf, h->in, n);
    memmove(h->in, h->in + n, h->inlen - n);
    h->inlen -= n;
    if (term_echo) {
        /* The substrate terminal echoes what is typed into it (a pty in
         * canonical mode: printable as itself, CR/LF as CR LF). The remote
         * server already echoed it, so that copy is dropped from the output.
         * A control character ends the prediction (its echo form varies). */
        for (size_t i = 0; i < n; i++) {
            uint8_t b = buf[i];
            if (b == '\r' || b == '\n') {
                if (h->echolen + 2 > DNET_CTH_ECHO_MAX) break;
                h->echo[h->echolen++] = '\r';
                h->echo[h->echolen++] = '\n';
            } else if (b >= 0x20 && b < 0x7f) {
                if (h->echolen + 1 > DNET_CTH_ECHO_MAX) break;
                h->echo[h->echolen++] = b;
            } else {
                break;
            }
        }
    }
    return n;
}

int dnet_cth_close(struct dnet_cth *h)
{
    if (!h) return DNET_CTH_EINVAL;
    if (h->state == DNET_CTH_S_UNBOUND || h->state == DNET_CTH_S_IDLE) {
        h->state = DNET_CTH_S_UNBOUND;
        return DNET_CTH_OK;
    }
    /* Whatever the session said last ("logged out at ...") goes out first. */
    if (h->state == DNET_CTH_S_BOUND && h->outlen) {
        size_t k = h->outlen;
        if (txq_free(h) < writes_needed(h, k) + 1) {
            size_t fit = (size_t)(txq_free(h) > 1 ? txq_free(h) - 1 : 0) * write_chunk(h);
            k = k < fit ? k : fit;                /* bounded: keep what fits */
        }
        if (k && q_write(h, h->out, k) == DNET_CTH_OK) out_consume(h, k);
    }
    uint8_t m[3];
    size_t ml = 0;
    int rc = dnet_cth_unbind_build(DNET_CTH_UNBIND_USER, m, sizeof m, &ml);
    if (rc == DNET_CTH_OK) rc = q_seg(h, m, ml);
    h->state = DNET_CTH_S_UNBOUND;
    h->read_active = 0;
    return rc;
}

int dnet_cth_tx_pop(struct dnet_cth *h, uint8_t *buf, size_t cap, size_t *outlen)
{
    if (!h || !buf || !h->txcount) return 0;
    size_t l = h->txlen[h->txhead];
    if (l > cap) return 0;
    memcpy(buf, h->txq[h->txhead], l);
    if (outlen) *outlen = l;
    h->txhead = (h->txhead + 1) % DNET_CTH_TXQ;
    h->txcount--;
    return 1;
}

int dnet_cth_is_bound(const struct dnet_cth *h) { return h && h->state == DNET_CTH_S_BOUND; }
int dnet_cth_is_over(const struct dnet_cth *h) { return !h || h->state == DNET_CTH_S_UNBOUND; }
