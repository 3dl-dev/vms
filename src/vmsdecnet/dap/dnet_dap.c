/*
 * dnet_dap.c - DECnet Phase IV DAP message codec (rd vms-8c2). See dnet_dap.h
 * for the clean-room provenance (Rule 8): the message set + generic framing are
 * the PUBLIC DAP spec; the SEQUENCE and the carried values (filename, resolved
 * full spec, owner UIC, verbatim records) are the docs/oracle/vax-copy-fal-dap.*
 * ground truth. Pure byte library: no socket, no allocation, fully bounded.
 *
 * WIRE FRAMING (public spec generic message, LENGTH-present form this codec
 * uses so a blocked NSP segment is walkable):
 *
 *     OPERATOR(1) FLAGS(1) LENGTH(1) <LENGTH bytes of message body>
 *
 * FLAGS is always DNET_DAP_FLAG_LENGTH here. LENGTH counts the body bytes that
 * follow it (spec: the message length field counts the remaining message). A
 * counted "image" field is 1 length byte + that many bytes (spec image field).
 */
#include "dnet_dap.h"

#include <string.h>

/* ---- bounded body readers/writers (offset into a fixed body buffer) ------- */

static int body_get_u8(const uint8_t *b, size_t blen, size_t *off, uint8_t *v)
{
    if (*off + 1 > blen) return DNET_DAP_ETRUNC;
    *v = b[*off]; *off += 1; return DNET_DAP_OK;
}

static int body_get_u16(const uint8_t *b, size_t blen, size_t *off, uint16_t *v)
{
    if (*off + 2 > blen) return DNET_DAP_ETRUNC;
    /* DAP integers are little-endian on the wire (VAX byte order). */
    *v = (uint16_t)(b[*off] | (b[*off + 1] << 8));
    *off += 2; return DNET_DAP_OK;
}

static int body_get_u32(const uint8_t *b, size_t blen, size_t *off, uint32_t *v)
{
    if (*off + 4 > blen) return DNET_DAP_ETRUNC;
    *v = (uint32_t)b[*off] | ((uint32_t)b[*off + 1] << 8) |
         ((uint32_t)b[*off + 2] << 16) | ((uint32_t)b[*off + 3] << 24);
    *off += 4; return DNET_DAP_OK;
}

/* Read a counted image field into a NUL-terminated string. Refuses (does not
 * clip) a field that would exceed `dstcap-1` or run past the body. */
static int body_get_str(const uint8_t *b, size_t blen, size_t *off,
                        char *dst, size_t dstcap)
{
    uint8_t n;
    int rc = body_get_u8(b, blen, off, &n);
    if (rc != DNET_DAP_OK) return rc;
    if ((size_t)n + 1 > dstcap) return DNET_DAP_EBADLEN;
    if (*off + n > blen) return DNET_DAP_ETRUNC;
    if (n) memcpy(dst, b + *off, n);
    dst[n] = '\0';
    *off += n;
    return DNET_DAP_OK;
}

/* Read a counted binary field (records may carry NULs). */
static int body_get_bytes(const uint8_t *b, size_t blen, size_t *off,
                         uint8_t *dst, size_t dstcap, uint16_t *outlen)
{
    uint16_t n;
    int rc = body_get_u16(b, blen, off, &n);
    if (rc != DNET_DAP_OK) return rc;
    if (n > dstcap) return DNET_DAP_EBADLEN;
    if (*off + n > blen) return DNET_DAP_ETRUNC;
    if (n) memcpy(dst, b + *off, n);
    *off += n;
    *outlen = n;
    return DNET_DAP_OK;
}

static int put_u8(uint8_t *b, size_t cap, size_t *off, uint8_t v)
{
    if (*off + 1 > cap) return DNET_DAP_ENOSPACE;
    b[*off] = v; *off += 1; return DNET_DAP_OK;
}
static int put_u16(uint8_t *b, size_t cap, size_t *off, uint16_t v)
{
    if (*off + 2 > cap) return DNET_DAP_ENOSPACE;
    b[*off] = (uint8_t)(v & 0xff); b[*off + 1] = (uint8_t)(v >> 8);
    *off += 2; return DNET_DAP_OK;
}
static int put_u32(uint8_t *b, size_t cap, size_t *off, uint32_t v)
{
    if (*off + 4 > cap) return DNET_DAP_ENOSPACE;
    b[*off] = (uint8_t)(v & 0xff); b[*off + 1] = (uint8_t)((v >> 8) & 0xff);
    b[*off + 2] = (uint8_t)((v >> 16) & 0xff); b[*off + 3] = (uint8_t)((v >> 24) & 0xff);
    *off += 4; return DNET_DAP_OK;
}
static int put_str(uint8_t *b, size_t cap, size_t *off, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n > 255) return DNET_DAP_EINVAL;
    if (*off + 1 + n > cap) return DNET_DAP_ENOSPACE;
    b[*off] = (uint8_t)n;
    if (n) memcpy(b + *off + 1, s, n);
    *off += 1 + n;
    return DNET_DAP_OK;
}

/* ---- encode -------------------------------------------------------------- */

int dnet_dap_encode(const struct dnet_dap_msg *msg,
                    uint8_t *buf, size_t cap, size_t *outlen)
{
    if (!msg || !buf) return DNET_DAP_EINVAL;

    uint8_t body[DNET_DAP_MAX_MSG];
    size_t  boff = 0;
    int rc = DNET_DAP_OK;

    switch (msg->op) {
    case DNET_DAP_CONFIG:
        rc = put_u16(body, sizeof body, &boff, msg->u.config.bufsiz);
        if (!rc) rc = put_u8(body, sizeof body, &boff, msg->u.config.ostype);
        if (!rc) rc = put_u8(body, sizeof body, &boff, msg->u.config.filesys);
        if (!rc) rc = put_u8(body, sizeof body, &boff, msg->u.config.version);
        break;
    case DNET_DAP_ATTRIBUTES:
        rc = put_u8(body, sizeof body, &boff, msg->u.attr.org);
        if (!rc) rc = put_u8(body, sizeof body, &boff, msg->u.attr.rfm);
        if (!rc) rc = put_u8(body, sizeof body, &boff, msg->u.attr.rat);
        if (!rc) rc = put_u16(body, sizeof body, &boff, msg->u.attr.mrs);
        if (!rc) rc = put_u32(body, sizeof body, &boff, msg->u.attr.alq);
        break;
    case DNET_DAP_ACCESS:
        rc = put_u8(body, sizeof body, &boff, msg->u.access.accfunc);
        if (!rc) rc = put_str(body, sizeof body, &boff, msg->u.access.filespec);
        break;
    case DNET_DAP_CONTROL:
        rc = put_u8(body, sizeof body, &boff, msg->u.control.ctlfunc);
        break;
    case DNET_DAP_NAME:
        rc = put_u8(body, sizeof body, &boff, msg->u.name.nametype);
        if (!rc) rc = put_str(body, sizeof body, &boff, msg->u.name.namespec);
        break;
    case DNET_DAP_DATA:
        if (msg->u.data.reclen > DNET_DAP_MAX_REC) return DNET_DAP_EINVAL;
        rc = put_u16(body, sizeof body, &boff, msg->u.data.reclen);
        if (!rc) {
            if (boff + msg->u.data.reclen > sizeof body) return DNET_DAP_ENOSPACE;
            if (msg->u.data.reclen) memcpy(body + boff, msg->u.data.rec, msg->u.data.reclen);
            boff += msg->u.data.reclen;
        }
        break;
    case DNET_DAP_STATUS:
        rc = put_u16(body, sizeof body, &boff, msg->u.status.stscode);
        break;
    case DNET_DAP_ACCESS_COMPLETE:
    case DNET_DAP_CONTINUE:
    case DNET_DAP_ACKNOWLEDGE:
        rc = put_u8(body, sizeof body, &boff, msg->u.complete.func);
        break;
    default:
        return DNET_DAP_EINVAL;   /* refuse to encode an unknown operator */
    }
    if (rc != DNET_DAP_OK) return rc;

    /* body length must fit the single-byte LENGTH field of this codec's frame. */
    if (boff > 255) return DNET_DAP_ENOSPACE;

    size_t off = 0;
    rc = put_u8(buf, cap, &off, (uint8_t)msg->op);
    if (!rc) rc = put_u8(buf, cap, &off, DNET_DAP_FLAG_LENGTH);
    if (!rc) rc = put_u8(buf, cap, &off, (uint8_t)boff);
    if (rc != DNET_DAP_OK) return rc;
    if (off + boff > cap) return DNET_DAP_ENOSPACE;
    if (boff) memcpy(buf + off, body, boff);
    off += boff;

    if (outlen) *outlen = off;
    return DNET_DAP_OK;
}

/* ---- decode -------------------------------------------------------------- */

int dnet_dap_decode(const uint8_t *buf, size_t len,
                    struct dnet_dap_msg *out, size_t *consumed)
{
    if (!buf || !out) return DNET_DAP_EINVAL;
    memset(out, 0, sizeof(*out));

    /* Generic header: OPERATOR, FLAGS, LENGTH. This codec only produces (and
     * therefore only accepts) the LENGTH-present shape -- any other FLAGS is
     * refused rather than guessed at. */
    if (len < 3) return DNET_DAP_ETRUNC;
    uint8_t operator = buf[0];
    uint8_t flags    = buf[1];
    uint8_t blen     = buf[2];
    if (flags != DNET_DAP_FLAG_LENGTH) return DNET_DAP_EINVAL;
    if ((size_t)3 + blen > len) return DNET_DAP_ETRUNC;

    const uint8_t *body = buf + 3;
    size_t boff = 0;
    int rc = DNET_DAP_OK;

    switch (operator) {
    case DNET_DAP_CONFIG:
        out->op = DNET_DAP_CONFIG;
        rc = body_get_u16(body, blen, &boff, &out->u.config.bufsiz);
        if (!rc) rc = body_get_u8(body, blen, &boff, &out->u.config.ostype);
        if (!rc) rc = body_get_u8(body, blen, &boff, &out->u.config.filesys);
        if (!rc) rc = body_get_u8(body, blen, &boff, &out->u.config.version);
        break;
    case DNET_DAP_ATTRIBUTES:
        out->op = DNET_DAP_ATTRIBUTES;
        rc = body_get_u8(body, blen, &boff, &out->u.attr.org);
        if (!rc) rc = body_get_u8(body, blen, &boff, &out->u.attr.rfm);
        if (!rc) rc = body_get_u8(body, blen, &boff, &out->u.attr.rat);
        if (!rc) rc = body_get_u16(body, blen, &boff, &out->u.attr.mrs);
        if (!rc) rc = body_get_u32(body, blen, &boff, &out->u.attr.alq);
        break;
    case DNET_DAP_ACCESS:
        out->op = DNET_DAP_ACCESS;
        rc = body_get_u8(body, blen, &boff, &out->u.access.accfunc);
        if (!rc) rc = body_get_str(body, blen, &boff, out->u.access.filespec,
                                   sizeof out->u.access.filespec);
        break;
    case DNET_DAP_CONTROL:
        out->op = DNET_DAP_CONTROL;
        rc = body_get_u8(body, blen, &boff, &out->u.control.ctlfunc);
        break;
    case DNET_DAP_NAME:
        out->op = DNET_DAP_NAME;
        rc = body_get_u8(body, blen, &boff, &out->u.name.nametype);
        if (!rc) rc = body_get_str(body, blen, &boff, out->u.name.namespec,
                                   sizeof out->u.name.namespec);
        break;
    case DNET_DAP_DATA:
        out->op = DNET_DAP_DATA;
        rc = body_get_bytes(body, blen, &boff, out->u.data.rec,
                           sizeof out->u.data.rec, &out->u.data.reclen);
        break;
    case DNET_DAP_STATUS:
        out->op = DNET_DAP_STATUS;
        rc = body_get_u16(body, blen, &boff, &out->u.status.stscode);
        break;
    case DNET_DAP_ACCESS_COMPLETE:
    case DNET_DAP_CONTINUE:
    case DNET_DAP_ACKNOWLEDGE:
        out->op = (enum dnet_dap_op)operator;
        rc = body_get_u8(body, blen, &boff, &out->u.complete.func);
        break;
    default:
        /* A well-formed frame of an operator this rung does not serve: report
         * it honestly (op UNKNOWN) and consume it so the caller can refuse the
         * transfer cleanly -- never fault on it (INV-6, never crash a peer). */
        out->op = DNET_DAP_MSG_UNKNOWN;
        rc = DNET_DAP_OK;
        break;
    }
    if (rc != DNET_DAP_OK) { memset(out, 0, sizeof(*out)); return rc; }

    if (consumed) *consumed = (size_t)3 + blen;
    return DNET_DAP_OK;
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
    case DNET_DAP_NAME:            return "NAME";
    case DNET_DAP_MSG_UNKNOWN:     default: return "UNKNOWN";
    }
}
