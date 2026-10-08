/*
 * dnet_dap.c - DECnet Phase IV DAP message codec (rd vms-8c2, re-grounded on
 * the public DAP 5.6 spec + live real-VMS FAL captures by rd vms-a8a). See
 * dnet_dap.h for the clean-room provenance. Pure byte library: no socket, no
 * allocation, fully bounded.
 *
 * FIELD TYPES (DAP 5.6 sec. 3.1 notation):
 *   B-n   n-byte little-endian binary
 *   EX-n  extensible field: up to n bytes, bit 7 of each byte = "another byte
 *         follows", the low 7 bits of each byte are the next 7 bits of value
 *   I-n   image field: one count byte (0..n) then that many bytes
 * A message is OPERATOR (TYPE, FLAGS) [STREAMID] [LENGTH [LEN256]] [BITCNT]
 * [SYSPEC] OPERAND. Without FLAGS.LENGTH the operand runs to the end of the
 * Session Control buffer (the NSP segment).
 */
#include "dnet_dap.h"

#include <string.h>

/* ---- bounded readers over an operand [b, b+blen) ------------------------- */

struct rd { const uint8_t *b; size_t len, off; };

static int rd_more(const struct rd *r) { return r->off < r->len; }

static int rd_b(struct rd *r, size_t n, uint64_t *v)
{
    if (n > 8 || r->off + n > r->len) return DNET_DAP_ETRUNC;
    uint64_t x = 0;
    for (size_t i = 0; i < n; i++) x |= (uint64_t)r->b[r->off + i] << (8 * i);
    r->off += n;
    *v = x;
    return DNET_DAP_OK;
}

static int rd_b8(struct rd *r, uint8_t *v)
{ uint64_t x; int rc = rd_b(r, 1, &x); if (!rc) *v = (uint8_t)x; return rc; }
static int rd_b16(struct rd *r, uint16_t *v)
{ uint64_t x; int rc = rd_b(r, 2, &x); if (!rc) *v = (uint16_t)x; return rc; }

/* EX-n: at most `maxn` bytes; the last byte must clear bit 7. Bits beyond 64
 * are bounded-and-dropped (no field OVMX uses goes past bit 63). Optionally
 * hands back the raw bytes. */
static int rd_ex(struct rd *r, size_t maxn, uint64_t *v, uint8_t *raw, uint8_t *rawlen)
{
    uint64_t x = 0;
    for (size_t i = 0; i < maxn; i++) {
        if (r->off >= r->len) return DNET_DAP_ETRUNC;
        uint8_t c = r->b[r->off++];
        if (7 * i < 64) x |= (uint64_t)(c & 0x7f) << (7 * i);
        if (raw) raw[i] = c;
        if (!(c & 0x80)) {
            if (rawlen) *rawlen = (uint8_t)(i + 1);
            *v = x;
            return DNET_DAP_OK;
        }
    }
    return DNET_DAP_EBADLEN;          /* ran past the field's declared maximum */
}

/* I-n image field as raw bytes into dst (cap >= n). */
static int rd_img(struct rd *r, size_t maxn, uint8_t *dst, size_t cap, uint8_t *outlen)
{
    if (r->off >= r->len) return DNET_DAP_ETRUNC;
    uint8_t n = r->b[r->off];
    if (n > maxn || n > cap) return DNET_DAP_EBADLEN;
    if (r->off + 1 + n > r->len) return DNET_DAP_ETRUNC;
    if (n) memcpy(dst, r->b + r->off + 1, n);
    r->off += 1 + (size_t)n;
    *outlen = n;
    return DNET_DAP_OK;
}

/* I-n image field as a NUL-terminated string. */
static int rd_str(struct rd *r, size_t maxn, char *dst, size_t cap)
{
    uint8_t n = 0;
    if (cap == 0) return DNET_DAP_EINVAL;
    int rc = rd_img(r, maxn, (uint8_t *)dst, cap - 1, &n);
    if (rc) return rc;
    dst[n] = '\0';
    return DNET_DAP_OK;
}

/* I-n image field holding a little-endian number (n <= 8). */
static int rd_inum(struct rd *r, size_t maxn, uint64_t *v, uint8_t *outlen)
{
    uint8_t tmp[8], n = 0;
    if (maxn > 8) maxn = 8;
    int rc = rd_img(r, maxn, tmp, sizeof tmp, &n);
    if (rc) return rc;
    uint64_t x = 0;
    for (uint8_t i = 0; i < n; i++) x |= (uint64_t)tmp[i] << (8 * i);
    *v = x;
    if (outlen) *outlen = n;
    return DNET_DAP_OK;
}

/* ---- bounded writers ------------------------------------------------------ */

struct wr { uint8_t *b; size_t cap, off; int err; };

static void wr_bytes(struct wr *w, const void *p, size_t n)
{
    if (w->err) return;
    if (w->off + n > w->cap) { w->err = DNET_DAP_ENOSPACE; return; }
    if (n) memcpy(w->b + w->off, p, n);
    w->off += n;
}
static void wr_b(struct wr *w, uint64_t v, size_t n)
{
    uint8_t t[8];
    for (size_t i = 0; i < n; i++) t[i] = (uint8_t)(v >> (8 * i));
    wr_bytes(w, t, n);
}
static void wr_ex(struct wr *w, uint64_t v, size_t maxn)
{
    uint8_t t[DNET_DAP_MAX_EX];
    size_t n = 0;
    do {
        if (n >= maxn || n >= sizeof t) { if (!w->err) w->err = DNET_DAP_EINVAL; return; }
        t[n++] = (uint8_t)(v & 0x7f);
        v >>= 7;
    } while (v);
    for (size_t i = 0; i + 1 < n; i++) t[i] |= 0x80;
    wr_bytes(w, t, n);
}
static void wr_img(struct wr *w, const void *p, size_t n, size_t maxn)
{
    if (n > maxn || n > 255) { if (!w->err) w->err = DNET_DAP_EINVAL; return; }
    uint8_t c = (uint8_t)n;
    wr_bytes(w, &c, 1);
    wr_bytes(w, p, n);
}
static void wr_inum(struct wr *w, uint64_t v, size_t maxn)
{
    uint8_t t[8]; size_t n = 0;
    while (v && n < 8) { t[n++] = (uint8_t)v; v >>= 8; }
    wr_img(w, t, n, maxn);
}

#define BIT(m, b) (((m) >> (b)) & 1u)

/* ---- encode --------------------------------------------------------------- */

static void enc_operand(const struct dnet_dap_msg *m, struct wr *w)
{
    switch (m->op) {
    case DNET_DAP_CONFIG: {
        wr_b(w, m->u.config.bufsiz, 2);
        wr_b(w, m->u.config.ostype, 1);
        wr_b(w, m->u.config.filesys, 1);
        wr_b(w, m->u.config.vernum, 1);
        wr_b(w, m->u.config.econum, 1);
        wr_b(w, m->u.config.usrnum, 1);
        wr_b(w, m->u.config.softver, 1);
        wr_b(w, m->u.config.usrsoft, 1);
        size_t n = m->u.config.syscap_len;
        if (n == 0 || n > DNET_DAP_MAX_EX) { w->err = DNET_DAP_EINVAL; return; }
        wr_bytes(w, m->u.config.syscap, n);
        break;
    }
    case DNET_DAP_ATTRIBUTES: {
        uint64_t mn = m->u.attr.menu;
        if (mn >> (DNET_DAP_ATT_LAST_KNOWN + 1)) { w->err = DNET_DAP_EINVAL; return; }
        if (BIT(mn, DNET_DAP_ATT_RUNSYS)) { w->err = DNET_DAP_EUNSUP; return; }
        wr_ex(w, mn, 6);
        if (BIT(mn, DNET_DAP_ATT_DATATYPE)) wr_ex(w, m->u.attr.datatype, 2);
        if (BIT(mn, DNET_DAP_ATT_ORG)) wr_b(w, m->u.attr.org, 1);
        if (BIT(mn, DNET_DAP_ATT_RFM)) wr_b(w, m->u.attr.rfm, 1);
        if (BIT(mn, DNET_DAP_ATT_RAT)) wr_ex(w, m->u.attr.rat, 3);
        if (BIT(mn, DNET_DAP_ATT_BLS)) wr_b(w, m->u.attr.bls, 2);
        if (BIT(mn, DNET_DAP_ATT_MRS)) wr_b(w, m->u.attr.mrs, 2);
        if (BIT(mn, DNET_DAP_ATT_ALQ)) wr_inum(w, m->u.attr.alq, 5);
        if (BIT(mn, DNET_DAP_ATT_BKS)) wr_b(w, m->u.attr.bks, 1);
        if (BIT(mn, DNET_DAP_ATT_FSZ)) wr_b(w, m->u.attr.fsz, 1);
        if (BIT(mn, DNET_DAP_ATT_MRN)) wr_inum(w, m->u.attr.mrn, 5);
        if (BIT(mn, DNET_DAP_ATT_DEQ)) wr_b(w, m->u.attr.deq, 2);
        if (BIT(mn, DNET_DAP_ATT_FOP)) wr_ex(w, m->u.attr.fop, 6);
        if (BIT(mn, DNET_DAP_ATT_BSZ)) wr_b(w, m->u.attr.bsz, 1);
        if (BIT(mn, DNET_DAP_ATT_DEV)) wr_ex(w, m->u.attr.dev, 6);
        if (BIT(mn, DNET_DAP_ATT_SDC)) wr_ex(w, m->u.attr.sdc, 6);
        if (BIT(mn, DNET_DAP_ATT_LRL)) wr_b(w, m->u.attr.lrl, 2);
        if (BIT(mn, DNET_DAP_ATT_HBK)) wr_inum(w, m->u.attr.hbk, 5);
        if (BIT(mn, DNET_DAP_ATT_EBK)) wr_inum(w, m->u.attr.ebk, 5);
        if (BIT(mn, DNET_DAP_ATT_FFB)) wr_b(w, m->u.attr.ffb, 2);
        if (BIT(mn, DNET_DAP_ATT_SBN)) wr_inum(w, m->u.attr.sbn, 5);
        break;
    }
    case DNET_DAP_ACCESS: {
        size_t n = strnlen(m->u.access.filespec, sizeof m->u.access.filespec);
        wr_b(w, m->u.access.accfunc, 1);
        wr_ex(w, m->u.access.accopt, 5);
        wr_img(w, m->u.access.filespec, n, DNET_DAP_MAX_SPEC);
        /* Trailing optional fields: a later one forces the earlier ones. */
        int disp = m->u.access.have_display;
        int shr  = m->u.access.have_shr || disp;
        int fac  = m->u.access.have_fac || shr;
        if (fac)  wr_ex(w, m->u.access.fac, 3);
        if (shr)  wr_ex(w, m->u.access.shr, 3);
        if (disp) wr_ex(w, m->u.access.display, 4);
        break;
    }
    case DNET_DAP_CONTROL: {
        uint64_t mn = m->u.control.menu;
        if (BIT(mn, 4) || BIT(mn, 6) || (mn >> 7)) { w->err = DNET_DAP_EUNSUP; return; }
        wr_b(w, m->u.control.ctlfunc, 1);
        wr_ex(w, mn, 4);
        if (BIT(mn, 0)) wr_b(w, m->u.control.rac, 1);
        if (BIT(mn, 1)) wr_img(w, m->u.control.key, m->u.control.keylen, 255);
        if (BIT(mn, 2)) wr_b(w, m->u.control.krf, 1);
        if (BIT(mn, 3)) wr_ex(w, m->u.control.rop, 6);
        if (BIT(mn, 5)) wr_ex(w, m->u.control.display, 4);
        break;
    }
    case DNET_DAP_CONTINUE:
        wr_b(w, m->u.cont.confunc, 1);
        break;
    case DNET_DAP_ACKNOWLEDGE:
        break;
    case DNET_DAP_ACCESS_COMPLETE: {
        wr_b(w, m->u.complete.cmpfunc, 1);
        int chk = m->u.complete.have_check;
        int fop = m->u.complete.have_fop || chk;
        if (fop) wr_ex(w, m->u.complete.fop, 6);
        if (chk) wr_b(w, m->u.complete.check, 2);
        break;
    }
    case DNET_DAP_DATA:
        if (m->u.data.reclen > DNET_DAP_MAX_REC) { w->err = DNET_DAP_EINVAL; return; }
        wr_inum(w, m->u.data.recnum, 8);
        wr_bytes(w, m->u.data.rec, m->u.data.reclen);
        break;
    case DNET_DAP_STATUS:
        wr_b(w, m->u.status.stscode, 2);
        if (m->u.status.have_stv) {
            wr_inum(w, m->u.status.rfa, 8);
            wr_inum(w, m->u.status.recnum, 8);
            wr_inum(w, m->u.status.stv, 8);
        }
        break;
    case DNET_DAP_NAME: {
        size_t n = strnlen(m->u.name.namespec, sizeof m->u.name.namespec);
        wr_ex(w, m->u.name.nametype, 3);
        wr_img(w, m->u.name.namespec, n, DNET_DAP_MAX_SPEC);
        break;
    }
    case DNET_DAP_SUMMARY:
        break;                         /* empty operand (see dnet_dap.h)      */
    case DNET_DAP_DATETIME: {
        uint64_t mn = m->u.datetime.menu;
        if (mn >> 4) { w->err = DNET_DAP_EUNSUP; return; }
        wr_ex(w, mn, 6);
        const char *d[3] = { m->u.datetime.cdt, m->u.datetime.rdt, m->u.datetime.edt };
        for (int i = 0; i < 3; i++) {
            if (!BIT(mn, i)) continue;
            /* A-18: exactly 18 characters, never a counted field. */
            if (strnlen(d[i], DNET_DAP_DATE_LEN + 1) != DNET_DAP_DATE_LEN) {
                w->err = DNET_DAP_EINVAL; return;
            }
            wr_bytes(w, d[i], DNET_DAP_DATE_LEN);
        }
        if (BIT(mn, 3)) wr_b(w, m->u.datetime.rvn, 2);
        break;
    }
    case DNET_DAP_PROTECTION: {
        uint64_t mn = m->u.prot.menu;
        if (mn >> 5) { w->err = DNET_DAP_EUNSUP; return; }
        wr_ex(w, mn, 6);
        if (BIT(mn, 0))
            wr_img(w, m->u.prot.owner, strnlen(m->u.prot.owner, sizeof m->u.prot.owner), 40);
        if (BIT(mn, 1)) wr_ex(w, m->u.prot.psys, 3);
        if (BIT(mn, 2)) wr_ex(w, m->u.prot.pown, 3);
        if (BIT(mn, 3)) wr_ex(w, m->u.prot.pgrp, 3);
        if (BIT(mn, 4)) wr_ex(w, m->u.prot.pwld, 3);
        break;
    }
    default:
        w->err = DNET_DAP_EINVAL;   /* never encode a type OVMX does not serve */
        break;
    }
}

int dnet_dap_encode(const struct dnet_dap_msg *msg, int with_length,
                    uint8_t *buf, size_t cap, size_t *outlen)
{
    if (!msg || !buf) return DNET_DAP_EINVAL;
    uint8_t body[DNET_DAP_MAX_MSG];
    struct wr w = { body, sizeof body, 0, 0 };
    enc_operand(msg, &w);
    if (w.err) return w.err;

    struct wr o = { buf, cap, 0, 0 };
    uint8_t hdr[4]; size_t hn = 0;
    hdr[hn++] = (uint8_t)msg->op;
    if (with_length) {
        if (w.off > 0xffff) return DNET_DAP_ENOSPACE;
        uint8_t fl = DNET_DAP_FLAG_LENGTH | (w.off > 255 ? DNET_DAP_FLAG_LEN256 : 0);
        hdr[hn++] = fl;
        hdr[hn++] = (uint8_t)(w.off & 0xff);
        if (fl & DNET_DAP_FLAG_LEN256) hdr[hn++] = (uint8_t)(w.off >> 8);
    } else {
        hdr[hn++] = 0;
    }
    wr_bytes(&o, hdr, hn);
    wr_bytes(&o, body, w.off);
    if (o.err) return o.err;
    if (outlen) *outlen = o.off;
    return DNET_DAP_OK;
}

/* ---- decode --------------------------------------------------------------- */

static int dec_operand(struct dnet_dap_msg *m, struct rd *r)
{
    int rc = DNET_DAP_OK;
    switch (m->type) {
    case DNET_DAP_CONFIG: {
        m->op = DNET_DAP_CONFIG;
        uint64_t v;
        if ((rc = rd_b16(r, &m->u.config.bufsiz))) return rc;
        if ((rc = rd_b8(r, &m->u.config.ostype))) return rc;
        if ((rc = rd_b8(r, &m->u.config.filesys))) return rc;
        if ((rc = rd_b8(r, &m->u.config.vernum))) return rc;
        if ((rc = rd_b8(r, &m->u.config.econum))) return rc;
        if ((rc = rd_b8(r, &m->u.config.usrnum))) return rc;
        if ((rc = rd_b8(r, &m->u.config.softver))) return rc;
        if ((rc = rd_b8(r, &m->u.config.usrsoft))) return rc;
        /* Spec 5.1 NOTE: a SYSCAP longer than EX-12 from a later DAP version
         * is tolerated -- keep the first 12 bytes, bound the rest. */
        size_t i = 0;
        for (;;) {
            if (r->off >= r->len) return DNET_DAP_ETRUNC;
            uint8_t c = r->b[r->off++];
            if (i < DNET_DAP_MAX_EX) m->u.config.syscap[i] = c;
            i++;
            if (!(c & 0x80)) break;
            if (i > 64) return DNET_DAP_EBADLEN;
        }
        m->u.config.syscap_len = (uint8_t)(i < DNET_DAP_MAX_EX ? i : DNET_DAP_MAX_EX);
        (void)v;
        return DNET_DAP_OK;
    }
    case DNET_DAP_ATTRIBUTES: {
        m->op = DNET_DAP_ATTRIBUTES;
        uint64_t mn, v;
        if ((rc = rd_ex(r, 6, &mn, NULL, NULL))) return rc;
        m->u.attr.menu = mn;
        if (BIT(mn, 0) && (rc = rd_ex(r, 2, &m->u.attr.datatype, NULL, NULL))) return rc;
        if (BIT(mn, 1) && (rc = rd_b8(r, &m->u.attr.org))) return rc;
        if (BIT(mn, 2) && (rc = rd_b8(r, &m->u.attr.rfm))) return rc;
        if (BIT(mn, 3) && (rc = rd_ex(r, 3, &m->u.attr.rat, NULL, NULL))) return rc;
        if (BIT(mn, 4) && (rc = rd_b16(r, &m->u.attr.bls))) return rc;
        if (BIT(mn, 5) && (rc = rd_b16(r, &m->u.attr.mrs))) return rc;
        if (BIT(mn, 6) && (rc = rd_inum(r, 5, &m->u.attr.alq, NULL))) return rc;
        if (BIT(mn, 7) && (rc = rd_b8(r, &m->u.attr.bks))) return rc;
        if (BIT(mn, 8) && (rc = rd_b8(r, &m->u.attr.fsz))) return rc;
        if (BIT(mn, 9) && (rc = rd_inum(r, 5, &m->u.attr.mrn, NULL))) return rc;
        if (BIT(mn, 10)) {                       /* RUNSYS I-40: bound + skip */
            char tmp[41];
            if ((rc = rd_str(r, 40, tmp, sizeof tmp))) return rc;
        }
        if (BIT(mn, 11) && (rc = rd_b16(r, &m->u.attr.deq))) return rc;
        if (BIT(mn, 12) && (rc = rd_ex(r, 6, &m->u.attr.fop, NULL, NULL))) return rc;
        if (BIT(mn, 13) && (rc = rd_b8(r, &m->u.attr.bsz))) return rc;
        if (BIT(mn, 14) && (rc = rd_ex(r, 6, &m->u.attr.dev, NULL, NULL))) return rc;
        if (BIT(mn, 15) && (rc = rd_ex(r, 6, &m->u.attr.sdc, NULL, NULL))) return rc;
        if (BIT(mn, 16) && (rc = rd_b16(r, &m->u.attr.lrl))) return rc;
        if (BIT(mn, 17) && (rc = rd_inum(r, 5, &m->u.attr.hbk, NULL))) return rc;
        if (BIT(mn, 18) && (rc = rd_inum(r, 5, &m->u.attr.ebk, NULL))) return rc;
        if (BIT(mn, 19) && (rc = rd_b16(r, &m->u.attr.ffb))) return rc;
        if (BIT(mn, 20) && (rc = rd_inum(r, 5, &m->u.attr.sbn, NULL))) return rc;
        (void)v;
        /* Menu bits past 20 are fields a later DAP version appends after
         * SBN (a real VMS V7.3 FAL sets bit 21); they are last in order, so
         * they are bounded by the operand and ignored, never misread. */
        return DNET_DAP_OK;
    }
    case DNET_DAP_ACCESS: {
        m->op = DNET_DAP_ACCESS;
        if ((rc = rd_b8(r, &m->u.access.accfunc))) return rc;
        if ((rc = rd_ex(r, 5, &m->u.access.accopt, NULL, NULL))) return rc;
        if ((rc = rd_str(r, DNET_DAP_MAX_SPEC, m->u.access.filespec,
                         sizeof m->u.access.filespec))) return rc;
        if (rd_more(r)) {
            if ((rc = rd_ex(r, 3, &m->u.access.fac, NULL, NULL))) return rc;
            m->u.access.have_fac = 1;
        }
        if (rd_more(r)) {
            if ((rc = rd_ex(r, 3, &m->u.access.shr, NULL, NULL))) return rc;
            m->u.access.have_shr = 1;
        }
        if (rd_more(r)) {
            if ((rc = rd_ex(r, 4, &m->u.access.display, NULL, NULL))) return rc;
            m->u.access.have_display = 1;
        }
        /* PASSWORD (I-40) and later-version fields: bounded by the operand,
         * not retained (a file password is never kept). */
        return DNET_DAP_OK;
    }
    case DNET_DAP_CONTROL: {
        m->op = DNET_DAP_CONTROL;
        uint64_t mn;
        if ((rc = rd_b8(r, &m->u.control.ctlfunc))) return rc;
        if (!rd_more(r)) return DNET_DAP_OK;      /* truncated after CTLFUNC */
        if ((rc = rd_ex(r, 4, &mn, NULL, NULL))) return rc;
        m->u.control.menu = mn;
        if (BIT(mn, 0) && (rc = rd_b8(r, &m->u.control.rac))) return rc;
        if (BIT(mn, 1) && (rc = rd_img(r, 255, m->u.control.key, sizeof m->u.control.key,
                                       &m->u.control.keylen))) return rc;
        if (BIT(mn, 2) && (rc = rd_b8(r, &m->u.control.krf))) return rc;
        if (BIT(mn, 3) && (rc = rd_ex(r, 6, &m->u.control.rop, NULL, NULL))) return rc;
        if (BIT(mn, 4)) { uint64_t h; if ((rc = rd_inum(r, 5, &h, NULL))) return rc; }
        if (BIT(mn, 5) && (rc = rd_ex(r, 4, &m->u.control.display, NULL, NULL))) return rc;
        return DNET_DAP_OK;
    }
    case DNET_DAP_CONTINUE:
        m->op = DNET_DAP_CONTINUE;
        return rd_b8(r, &m->u.cont.confunc);
    case DNET_DAP_ACKNOWLEDGE:
        m->op = DNET_DAP_ACKNOWLEDGE;
        return DNET_DAP_OK;
    case DNET_DAP_ACCESS_COMPLETE:
        m->op = DNET_DAP_ACCESS_COMPLETE;
        if ((rc = rd_b8(r, &m->u.complete.cmpfunc))) return rc;
        if (rd_more(r)) {
            if ((rc = rd_ex(r, 6, &m->u.complete.fop, NULL, NULL))) return rc;
            m->u.complete.have_fop = 1;
        }
        if (r->off + 2 <= r->len) {
            if ((rc = rd_b16(r, &m->u.complete.check))) return rc;
            m->u.complete.have_check = 1;
        }
        return DNET_DAP_OK;
    case DNET_DAP_DATA: {
        m->op = DNET_DAP_DATA;
        if ((rc = rd_inum(r, 8, &m->u.data.recnum, &m->u.data.recnum_len))) return rc;
        size_t n = r->len - r->off;
        if (n > DNET_DAP_MAX_REC) return DNET_DAP_EBADLEN;
        if (n) memcpy(m->u.data.rec, r->b + r->off, n);
        m->u.data.reclen = (uint16_t)n;
        r->off = r->len;
        return DNET_DAP_OK;
    }
    case DNET_DAP_STATUS:
        m->op = DNET_DAP_STATUS;
        if ((rc = rd_b16(r, &m->u.status.stscode))) return rc;
        if (rd_more(r) && (rc = rd_inum(r, 8, &m->u.status.rfa, NULL))) return rc;
        if (rd_more(r) && (rc = rd_inum(r, 8, &m->u.status.recnum, NULL))) return rc;
        if (rd_more(r)) {
            if ((rc = rd_inum(r, 8, &m->u.status.stv, NULL))) return rc;
            m->u.status.have_stv = 1;
        }
        return DNET_DAP_OK;
    case DNET_DAP_NAME:
        m->op = DNET_DAP_NAME;
        if ((rc = rd_ex(r, 3, &m->u.name.nametype, NULL, NULL))) return rc;
        return rd_str(r, DNET_DAP_MAX_SPEC, m->u.name.namespec, sizeof m->u.name.namespec);
    case DNET_DAP_DATETIME: {
        /* DAP 5.6 fields (CDT/RDT/EDT A-18, RVN B-2). Menu bits a later DAP
         * version adds (a VMS V7.3 FAL talking DAP 7 to its own kind sets
         * bits 7-8 with binary times) follow RVN; they are bounded by the
         * operand and not decoded. */
        m->op = DNET_DAP_DATETIME;
        uint64_t mn;
        if (!rd_more(r)) return DNET_DAP_OK;
        if ((rc = rd_ex(r, 6, &mn, NULL, NULL))) return rc;
        m->u.datetime.menu = mn;
        char *d[3] = { m->u.datetime.cdt, m->u.datetime.rdt, m->u.datetime.edt };
        for (int i = 0; i < 3; i++) {
            if (!BIT(mn, i)) continue;
            if (r->off + DNET_DAP_DATE_LEN > r->len) return DNET_DAP_ETRUNC;
            memcpy(d[i], r->b + r->off, DNET_DAP_DATE_LEN);
            d[i][DNET_DAP_DATE_LEN] = '\0';
            r->off += DNET_DAP_DATE_LEN;
        }
        if (BIT(mn, 3) && (rc = rd_b16(r, &m->u.datetime.rvn))) return rc;
        r->off = r->len;
        return DNET_DAP_OK;
    }
    case DNET_DAP_PROTECTION: {
        m->op = DNET_DAP_PROTECTION;
        uint64_t mn;
        if (!rd_more(r)) return DNET_DAP_OK;
        if ((rc = rd_ex(r, 6, &mn, NULL, NULL))) return rc;
        m->u.prot.menu = mn;
        if (BIT(mn, 0) && (rc = rd_str(r, 40, m->u.prot.owner, sizeof m->u.prot.owner))) return rc;
        if (BIT(mn, 1) && (rc = rd_ex(r, 3, &m->u.prot.psys, NULL, NULL))) return rc;
        if (BIT(mn, 2) && (rc = rd_ex(r, 3, &m->u.prot.pown, NULL, NULL))) return rc;
        if (BIT(mn, 3) && (rc = rd_ex(r, 3, &m->u.prot.pgrp, NULL, NULL))) return rc;
        if (BIT(mn, 4) && (rc = rd_ex(r, 3, &m->u.prot.pwld, NULL, NULL))) return rc;
        r->off = r->len;
        return DNET_DAP_OK;
    }
    case DNET_DAP_KEYDEF: case DNET_DAP_ALLOC: case DNET_DAP_SUMMARY:
    case DNET_DAP_ACL:
        /* Extended-attribute messages: a known type OVMX does not serve. The
         * operand is bounded (by LENGTH or the segment end) and skipped. */
        m->op = (enum dnet_dap_op)m->type;
        r->off = r->len;
        return DNET_DAP_OK;
    default:
        m->op = DNET_DAP_MSG_UNKNOWN;
        r->off = r->len;
        return DNET_DAP_OK;
    }
}

int dnet_dap_decode(const uint8_t *buf, size_t len,
                    struct dnet_dap_msg *out, size_t *consumed)
{
    if (!buf || !out) return DNET_DAP_EINVAL;
    memset(out, 0, sizeof *out);
    if (consumed) *consumed = 0;
    if (len < 2) return DNET_DAP_ETRUNC;

    size_t off = 0;
    uint8_t type = buf[off++], fl = buf[off++];
    if (fl & (DNET_DAP_FLAG_RSVD4 | 0x80)) { return DNET_DAP_EINVAL; }
    if (fl & DNET_DAP_FLAG_SEGMENTED)     { return DNET_DAP_EUNSUP; }
    if ((fl & DNET_DAP_FLAG_LEN256) && !(fl & DNET_DAP_FLAG_LENGTH)) return DNET_DAP_EINVAL;

    struct dnet_dap_msg m;
    memset(&m, 0, sizeof m);
    m.type = type; m.flags = fl;
    if (fl & DNET_DAP_FLAG_STREAMID) {
        if (off >= len) return DNET_DAP_ETRUNC;
        m.streamid = buf[off++];
    }
    size_t oplen = 0; int have_len = 0;
    if (fl & DNET_DAP_FLAG_LENGTH) {
        if (off >= len) return DNET_DAP_ETRUNC;
        oplen = buf[off++];
        if (fl & DNET_DAP_FLAG_LEN256) {
            if (off >= len) return DNET_DAP_ETRUNC;
            oplen |= (size_t)buf[off++] << 8;
        }
        have_len = 1;
    }
    if (fl & DNET_DAP_FLAG_BITCNT) {
        if (type != DNET_DAP_DATA) return DNET_DAP_EINVAL;
        if (off >= len) return DNET_DAP_ETRUNC;
        m.bitcnt = buf[off++];
        if (m.bitcnt > 7) return DNET_DAP_EINVAL;
    }
    if (fl & DNET_DAP_FLAG_SYSPEC) {           /* I-255, skipped (homogeneous) */
        if (off >= len) return DNET_DAP_ETRUNC;
        size_t sn = buf[off++];
        if (off + sn > len) return DNET_DAP_ETRUNC;
        off += sn;
    }
    if (have_len && off + oplen > len) return DNET_DAP_ETRUNC;
    if (!have_len) oplen = len - off;

    struct rd r = { buf + off, oplen, 0 };
    int rc = dec_operand(&m, &r);
    if (rc != DNET_DAP_OK) return rc;

    *out = m;
    if (consumed) *consumed = off + oplen;
    return DNET_DAP_OK;
}

int dnet_dap_syscap_has(const struct dnet_dap_msg *c, unsigned bit)
{
    if (!c || c->op != DNET_DAP_CONFIG) return 0;
    unsigned byte = bit / 7, b = bit % 7;
    if (byte >= c->u.config.syscap_len) return 0;
    for (unsigned i = 0; i < byte; i++)
        if (!(c->u.config.syscap[i] & 0x80)) return 0;   /* field ended early */
    return (c->u.config.syscap[byte] >> b) & 1u;
}

void dnet_dap_ovmx_config(struct dnet_dap_msg *m, uint16_t bufsiz)
{
    memset(m, 0, sizeof *m);
    m->op = DNET_DAP_CONFIG;
    m->u.config.bufsiz  = bufsiz;
    m->u.config.ostype  = DNET_DAP_OS_VAXVMS;
    m->u.config.filesys = DNET_DAP_FS_RMS32;
    m->u.config.vernum  = 5;        /* DAP 5.6: the version OVMX implements */
    m->u.config.econum  = 6;
    /* SYSCAP: exactly what OVMX serves (dnet_dap.h VERSION / SCOPE). */
    /* A real VMS COPY reading a remote file requires DIRECTORY LIST (it
     * $SEARCHes its input first) and asks for NAME; without bit 25 it refuses
     * the open "FAL-F-ACCFUNC, unsupported RMS service call" (rd vms-d85 lab). */
    /* rd vms-277a live bracket: a VMS client asks DIRECTORY/FULL for only the
     * main ATTRIBUTES unless SUMMARY / DATE AND TIME / PROTECTION are
     * advertised (bits 24, 26, 27 -- all served from the file header), and
     * refuses a remote RENAME "RMS-F-SUPPORT" without bit 37 and a DELETE ;*
     * "RMS-F-WLD" without bit 38. A VMS client resolves the wildcard itself
     * by a DIRECTORY LIST and then names each file explicitly (VAX<->VAX
     * capture, tests/lab/captures/decnet-fal-verbs-20261008/vax-to-vax-
     * sys-login/); OVMX serves a wildcard ERASE and refuses a wildcard OPEN
     * or RENAME with an honest STATUS (unsupported). */
    const unsigned caps[] = { DNET_DAP_CAP_SEQ_ORG, DNET_DAP_CAP_SEQ_XFER,
                              DNET_DAP_CAP_BLOCK_TO_RESP, DNET_DAP_CAP_LEN256,
                              DNET_DAP_CAP_SUMMARY, DNET_DAP_CAP_DIRLIST,
                              DNET_DAP_CAP_DATETIME, DNET_DAP_CAP_PROTECTION,
                              DNET_DAP_CAP_SEQ_RECORD, DNET_DAP_CAP_RENAME,
                              DNET_DAP_CAP_WILDCARD, DNET_DAP_CAP_NAME_MSG };
    unsigned maxbit = 0;
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++) {
        m->u.config.syscap[caps[i] / 7] |= (uint8_t)(1u << (caps[i] % 7));
        if (caps[i] > maxbit) maxbit = caps[i];
    }
    unsigned n = maxbit / 7 + 1;
    for (unsigned i = 0; i + 1 < n; i++) m->u.config.syscap[i] |= 0x80;
    m->u.config.syscap_len = (uint8_t)n;
}

const char *dnet_dap_op_name(enum dnet_dap_op op)
{
    switch (op) {
    case DNET_DAP_CONFIG:          return "CONFIGURATION";
    case DNET_DAP_ATTRIBUTES:      return "ATTRIBUTES";
    case DNET_DAP_ACCESS:          return "ACCESS";
    case DNET_DAP_CONTROL:         return "CONTROL";
    case DNET_DAP_CONTINUE:        return "CONTINUE";
    case DNET_DAP_ACKNOWLEDGE:     return "ACKNOWLEDGE";
    case DNET_DAP_ACCESS_COMPLETE: return "ACCESS-COMPLETE";
    case DNET_DAP_DATA:            return "DATA";
    case DNET_DAP_STATUS:          return "STATUS";
    case DNET_DAP_KEYDEF:          return "KEY-DEFINITION";
    case DNET_DAP_ALLOC:           return "ALLOCATION";
    case DNET_DAP_SUMMARY:         return "SUMMARY";
    case DNET_DAP_DATETIME:        return "DATE-TIME";
    case DNET_DAP_PROTECTION:      return "PROTECTION";
    case DNET_DAP_NAME:            return "NAME";
    case DNET_DAP_ACL:             return "ACL";
    case DNET_DAP_MSG_UNKNOWN:     default: return "UNKNOWN";
    }
}
